/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"
#include "mlir/Dialect/StandardOps/Ops.h"  // TF:local_config_mlir
#include "mlir/IR/Attributes.h"  // TF:local_config_mlir
#include "mlir/IR/Builders.h"  // TF:local_config_mlir
#include "mlir/IR/Matchers.h"  // TF:local_config_mlir
#include "mlir/IR/OpImplementation.h"  // TF:local_config_mlir
#include "mlir/IR/PatternMatch.h"  // TF:local_config_mlir
#include "mlir/IR/StandardTypes.h"  // TF:local_config_mlir
#include "mlir/IR/TypeUtilities.h"  // TF:local_config_mlir
#include "mlir/Support/LLVM.h"  // TF:local_config_mlir
#include "mlir/Support/LogicalResult.h"  // TF:local_config_mlir
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"

namespace mlir {
namespace TFL {

//===----------------------------------------------------------------------===//
// TensorFlowLiteDialect
//===----------------------------------------------------------------------===//

TensorFlowLiteDialect::TensorFlowLiteDialect(mlir::MLIRContext *context)
    : Dialect(/*name=*/"tfl", context) {
  addOperations<
#define GET_OP_LIST
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.cc.inc"
      >();
}

//===----------------------------------------------------------------------===//
// Common support logic
//===----------------------------------------------------------------------===//

namespace {

// Returns true if the dimensions in `a` is a suffix of the ones in `b`.
// For example, dimensions {2}, {1, 2}, and {3, 1, 2} are all suffixes to
// {5, 4, 3, 1, 2}, while {1}, {5, 4}, and {1, 3, 2} are all not.
inline bool IsTrailingDimensions(ArrayRef<int64_t> a, ArrayRef<int64_t> b) {
  if (a.size() > b.size()) return false;

  return std::equal(a.rbegin(), a.rend(), b.rbegin());
}

// Returns true if it is a shaped type of f32 elements.
inline bool IsF32ShapedType(Type t) {
  if (auto shaped_type = t.dyn_cast_or_null<ShapedType>()) {
    return shaped_type.getElementType().isF32();
  }
  return false;
}

// Performs const folding `calculate` with broadcast behavior on the two
// attributes `operand1` and `operand2` and returns the result if possible.
// The two operands are expected to both be scalar values.
template <class AttrElementT,
          class ElementValueT = typename AttrElementT::ValueType,
          class CalculationT =
              llvm::function_ref<ElementValueT(ElementValueT, ElementValueT)>>
Attribute ConstFoldBinaryOpScalarScalar(Type result_type, Attribute operand1,
                                        Attribute operand2,
                                        const CalculationT &calculate) {
  auto lhs = operand1.cast<AttrElementT>();
  auto rhs = operand2.cast<AttrElementT>();

  assert(lhs.getType() == result_type && rhs.getType() == result_type &&
         "values of incompatible types should be caught by op verification");

  // TODO: Need to handle overflow/underflow cases.
  return AttrElementT::get(result_type,
                           calculate(lhs.getValue(), rhs.getValue()));
}

// Returns new shape with rank 'new_dims' with padded ones on the
// left if needed.
inline std::vector<int64_t> GetPaddedShape(ArrayRef<int64_t> old_shape,
                                           int new_dims) {
  std::vector<int64_t> new_shape(new_dims, 1);
  std::copy_backward(old_shape.begin(), old_shape.end(), new_shape.end());
  return new_shape;
}

// Helper method that given and 'current_index' representing
// index in broadcasted tensor, get the index in the flat original tensor.
// 'shape' is the original shape with padding to match result shape.
int64_t GetElementIndex(const std::vector<int64_t> &shape,
                        const std::vector<int64_t> &current_index) {
  int64_t ind = 0;
  int64_t mul = 1;
  for (int i = shape.size() - 1; i >= 0; --i) {
    ind += (current_index[i] % shape[i]) * mul;
    mul *= shape[i];
  }
  return ind;
}

// Helper method that increment index represented in 'current_index_ptr'
// in the shape of 'result_shape'.
void IncrementIndex(ArrayRef<int64_t> result_shape,
                    std::vector<int64_t> *current_index_ptr) {
  std::vector<int64_t> &current_index = *current_index_ptr;
  for (int i = result_shape.size() - 1; i >= 0; --i) {
    current_index[i]++;
    if (current_index[i] == result_shape[i]) {
      current_index[i] = 0;
    } else {
      break;
    }
  }
}

/// Performs const folding `calculate` with broadcast behavior on the two
/// attributes `operand1` and `operand2` and returns the result if possible.
/// This function assumes the both operands are verified to have value
/// attributes of broadcastable types.
template <class AttrElementT,
          class ElementValueT = typename AttrElementT::ValueType,
          class CalculationT =
              llvm::function_ref<ElementValueT(ElementValueT, ElementValueT)>>
Attribute ConstFoldBinaryOpDenseDense(Type result_type, DenseElementsAttr lhs,
                                      DenseElementsAttr rhs,
                                      const CalculationT &calculate) {
  auto type = OpTrait::util::getBroadcastedType(lhs.getType(), rhs.getType())
                  .dyn_cast_or_null<ShapedType>();
  if (!type) {
    return {};
  }

  const bool rhs_is_splat = rhs.isSplat();
  const bool lhs_is_splat = lhs.isSplat();

  // If both of them are splat, compute and return.
  if (lhs_is_splat && rhs_is_splat) {
    auto element_result = AttrElementT::get(
        type.getElementType(), calculate(lhs.getSplatValue<ElementValueT>(),
                                         rhs.getSplatValue<ElementValueT>()));
    if (!element_result) return {};

    return DenseElementsAttr::get(type, element_result);
  }

  auto num_elements = type.getNumElements();
  SmallVector<ElementValueT, 16> lhs_old_values;
  SmallVector<ElementValueT, 16> rhs_old_values;
  if (lhs_is_splat)
    lhs_old_values.push_back(lhs.getSplatValue<ElementValueT>());
  else
    lhs_old_values = llvm::to_vector<16>(lhs.getValues<ElementValueT>());
  if (rhs_is_splat)
    rhs_old_values.push_back(rhs.getSplatValue<ElementValueT>());
  else
    rhs_old_values = llvm::to_vector<16>(rhs.getValues<ElementValueT>());
  SmallVector<ElementValueT, 16> new_values;
  new_values.reserve(num_elements);
  const auto result_shape = type.getShape();
  std::vector<int64_t> current_index(type.getRank(), 0);
  // Create the new shape with ones padded to the left.
  std::vector<int64_t> lhs_new_shape =
      GetPaddedShape(lhs.getType().getShape(), type.getRank());
  std::vector<int64_t> rhs_new_shape =
      GetPaddedShape(rhs.getType().getShape(), type.getRank());

  // Add each pair of the corresponding values in the dense elements
  // attributes.
  for (int64_t i = 0; i < num_elements; ++i) {
    // current_index represents the index
    // in the N-dimension tensor. GetElementIndex returns
    // the index in the flat representation of the original tensor
    // to use.
    int64_t lhs_index =
        lhs_is_splat ? 0 : GetElementIndex(lhs_new_shape, current_index);
    int64_t rhs_index =
        rhs_is_splat ? 0 : GetElementIndex(rhs_new_shape, current_index);

    new_values.push_back(
        calculate(lhs_old_values[lhs_index], rhs_old_values[rhs_index]));
    IncrementIndex(result_shape, &current_index);
  }
  return DenseElementsAttr::get(type, new_values);
}

/// Performs const folding `calculate` with broadcast behavior on the two
/// attributes `operand1` and `operand2` and returns the result if possible.
/// This function assumes the two operands are verified to have value
/// attributes of broadcastable types.
template <class AttrElementT,
          class ElementValueT = typename AttrElementT::ValueType,
          class CalculationT =
              llvm::function_ref<ElementValueT(ElementValueT, ElementValueT)>>
Attribute ConstFoldBinaryOp(Type result_type, Attribute operand1,
                            Attribute operand2, const CalculationT &calculate,
                            bool is_commutative) {
  if (operand1.dyn_cast_or_null<AttrElementT>()) {
    // Scalar op scalar case
    if (operand2.dyn_cast_or_null<AttrElementT>())
      return ConstFoldBinaryOpScalarScalar<AttrElementT>(result_type, operand1,
                                                         operand2, calculate);
  } else if (operand1.dyn_cast_or_null<DenseElementsAttr>() &&
             operand2.dyn_cast_or_null<DenseElementsAttr>()) {
    return ConstFoldBinaryOpDenseDense<AttrElementT>(
        result_type, operand1.cast<DenseElementsAttr>(),
        operand2.cast<DenseElementsAttr>(), calculate);
  }

  // TODO: support other attribute kinds

  return {};
}

/// Performs const folding with broadcast behavior on the two attributes in
/// `operands` and returns the result if possible.
/// Depending on the given `resultType`, either `floatCalculate` or
/// `intCalculate` is chosen to conduct the calculate.
Attribute ConstFoldBinaryOp(
    Type result_type, ArrayRef<Attribute> operands,
    llvm::function_ref<APFloat(APFloat, APFloat)> float_calculate,
    llvm::function_ref<APInt(APInt, APInt)> int_calculate,
    bool is_commutative) {
  // Note: All types are wrapped in tensor types in TFlite. E.g., f32 is
  // represented as tensor<f32>. So we are only handling tensor types here.
  auto type = result_type.dyn_cast<ShapedType>();
  if (!type) return {};

  auto elemType = type.getElementType();

  if (elemType.isa<FloatType>())
    return ConstFoldBinaryOp<FloatAttr>(result_type, operands[0], operands[1],
                                        float_calculate, is_commutative);

  if (elemType.isa<IntegerType>())
    return ConstFoldBinaryOp<IntegerAttr>(result_type, operands[0], operands[1],
                                          int_calculate, is_commutative);

  return {};
}

/// Performs const folding a attributes `operand` and returns the result if
/// possible.
/// The function currently asserts that the `result_type` to be a f32 tensor
/// type.
/// TODO: Extend this function to handle integral tensor for ops like
/// "tfl.logical_not".
Attribute ConstFoldUnaryOp(Type result_type, Attribute operand,
                           llvm::function_ref<APFloat(APFloat)> calculate) {
  assert(IsF32ShapedType(result_type));
  auto result_shape_type = result_type.cast<ShapedType>();

  if (auto dense_elements = operand.dyn_cast_or_null<DenseElementsAttr>()) {
    SmallVector<APFloat, 16> new_values;
    const int num_elements = result_shape_type.getNumElements();
    new_values.reserve(num_elements);

    for (APFloat old_value : dense_elements.getValues<APFloat>()) {
      new_values.push_back(calculate(old_value));
    }

    return DenseElementsAttr::get(result_shape_type, new_values);
  }

  return {};
}

void buildComparisonBinOp(Builder *builder, OperationState &result, Value *lhs,
                          Value *rhs) {
  auto result_type =
      OpTrait::util::getBroadcastedType(lhs->getType(), rhs->getType());
  if (!result_type)
    emitError(result.location)
        << "non-broadcastable operands: " << lhs->getType() << " and "
        << rhs->getType();
  result.addOperands({lhs, rhs});
  // Comparison binary ops always return i1 tensor.
  if (auto shaped_type = result_type.dyn_cast<RankedTensorType>()) {
    auto result_shape = shaped_type.getShape();
    result.types.push_back(
        builder->getTensorType(result_shape, builder->getI1Type()));
  } else {
    result.types.push_back(builder->getTensorType(builder->getI1Type()));
  }
}

void buildFusedBroadcastableBinOp(Builder *builder, OperationState &result,
                                  Value *lhs, Value *rhs,
                                  StringAttr fused_activation_function) {
  auto result_type =
      OpTrait::util::getBroadcastedType(lhs->getType(), rhs->getType());

  if (!result_type)
    emitError(result.location)
        << "non-broadcastable operands: " << lhs->getType() << " and "
        << rhs->getType();

  result.addOperands({lhs, rhs});
  result.addAttribute("fused_activation_function", fused_activation_function);
  result.types.push_back(result_type);
}

}  // end anonymous namespace

//===----------------------------------------------------------------------===//
// AddOp
//===----------------------------------------------------------------------===//

OpFoldResult AddOp::fold(ArrayRef<Attribute> operands) {
  // TODO(b/142478136): Handle fused ops.
  if (fused_activation_function() != "NONE") return {};
  return ConstFoldBinaryOp(
      getType(), operands, [](APFloat a, APFloat b) { return a + b; },
      [](APInt a, APInt b) { return a + b; }, getOperation()->isCommutative());
}

//===----------------------------------------------------------------------===//
// ConcatenationOp
//===----------------------------------------------------------------------===//
// TODO(ashwinm): Implement shape inference for Concatenation

namespace {

int64_t GetConcatenationOpAxis(ConcatenationOp op) {
  auto output_type = op.output()->getType().cast<RankedTensorType>();
  int64_t axis = op.axis().getSExtValue();
  if (axis < 0) axis += output_type.getRank();
  return axis;
}

// Verify operand types and the result type:
//
// 1. Operand type ranks must be equal to the output type rank.
//
// 2. Operand dimension sizes (except dimension `axis`) must be equal to
//    previously seen dimension sizes of the same dimension.
//
// 3. Sum of operand dimension sizes of the `axis` dimension must be equal to
//    the dimension size of the `axis` dimension of output.
//
// Note: If an operand has unranked tensor type or has dynamic dimension size,
// those dimensions will be skipped.
LogicalResult VerifyConcatenationOpTypes(Operation *op,
                                         RankedTensorType output_type,
                                         ArrayRef<TensorType> operand_types,
                                         int64_t axis) {
  const int64_t output_rank = output_type.getRank();

  constexpr int64_t kDynamicSize = -1;
  SmallVector<int64_t, 4> result_dim_sizes_loc(output_rank, -1);
  SmallVector<int64_t, 4> result_dim_sizes(output_type.getShape().begin(),
                                           output_type.getShape().end());
  result_dim_sizes[axis] = 0;

  auto FormatLoc = [&result_dim_sizes_loc](int64_t dim) {
    const int64_t loc = result_dim_sizes_loc[dim];
    if (loc == -1) return std::string("output");
    return llvm::formatv("operand #{0}", loc).str();
  };

  for (auto operand : llvm::enumerate(operand_types)) {
    auto operand_type = operand.value().dyn_cast<RankedTensorType>();
    if (!operand_type) {
      result_dim_sizes[axis] = kDynamicSize;
      continue;
    }

    const int64_t operand_rank = operand_type.getRank();
    if (operand_rank != output_rank)
      return op->emitOpError() << "rank of operand #" << operand.index()
                               << " must be equal to rank of output, expected "
                               << output_rank << ", got " << operand_rank;

    for (int64_t dim = 0; dim < output_rank; ++dim) {
      const int64_t operand_dim_size = operand_type.getDimSize(dim);
      const int64_t result_dim_size = result_dim_sizes[dim];

      if (dim == axis) {
        if (RankedTensorType::isDynamic(operand_dim_size) ||
            RankedTensorType::isDynamic(result_dim_size))
          result_dim_sizes[axis] = kDynamicSize;
        else
          result_dim_sizes[axis] += operand_dim_size;
        continue;
      }

      if (RankedTensorType::isDynamic(operand_dim_size)) continue;

      if (RankedTensorType::isDynamic(result_dim_size)) {
        result_dim_sizes[dim] = operand_dim_size;
        result_dim_sizes_loc[dim] = operand.index();
        continue;
      }

      if (result_dim_size != operand_dim_size)
        return op->emitOpError()
               << "dimension size of dimension #" << dim << " of operand #"
               << operand.index() << " must be equal to "
               << "dimension size of dimension #" << dim << " of "
               << FormatLoc(dim) << ", expected " << result_dim_size << ", got "
               << operand_dim_size;
    }
  }

  const int64_t output_concated_dim_size = output_type.getDimSize(axis);
  if (!RankedTensorType::isDynamic(output_concated_dim_size) &&
      !RankedTensorType::isDynamic(result_dim_sizes[axis]) &&
      result_dim_sizes[axis] != output_concated_dim_size)
    return op->emitOpError()
           << "dimension size of dimension #" << axis << " of output "
           << "must be equal to the sum of dimension sizes of dimension #"
           << axis << ", expected " << result_dim_sizes[axis] << ", got "
           << output_concated_dim_size;

  return success();
}

LogicalResult Verify(ConcatenationOp op) {
  auto output_type = op.output()->getType().dyn_cast<RankedTensorType>();

  // If the output type is unranked, there is nothing else to be verified.
  if (!output_type) return success();

  const int64_t axis = GetConcatenationOpAxis(op);
  if (axis < 0 || axis >= output_type.getRank())
    return op.emitOpError("concatenation dimension must be in [-rank, rank)");

  SmallVector<TensorType, 4> operand_types;
  for (Value *operand : op.values())
    operand_types.push_back(operand->getType().cast<TensorType>());

  return VerifyConcatenationOpTypes(op.getOperation(), output_type,
                                    operand_types, axis);
}

// Returns true when all operands are instances of DenseElementsAttr and the
// output type has a static shape.
bool IsConcatenationOpConstFoldable(ConcatenationOp op,
                                    ArrayRef<Attribute> operands,
                                    RankedTensorType output_type,
                                    int64_t axis) {
  if (operands.empty()) return false;
  if (!output_type.hasStaticShape()) return false;
  if (axis < 0) return false;

  return llvm::all_of(operands, [](Attribute operand) {
    return operand && operand.isa<DenseElementsAttr>();
  });
}

DenseElementsAttr ConstFoldConcatenateOpDense(ArrayRef<Attribute> operands,
                                              RankedTensorType output_type,
                                              int64_t axis) {
  const auto outer_dims = output_type.getShape().take_front(axis);
  const int64_t outer_size = std::accumulate(
      outer_dims.begin(), outer_dims.end(), 1, std::multiplies<int64_t>());

  const auto base_inner_dims = output_type.getShape().drop_front(axis + 1);
  const int64_t base_inner_size =
      std::accumulate(base_inner_dims.begin(), base_inner_dims.end(), 1,
                      std::multiplies<int64_t>());

  // Splits each input operand into outer_size pieces and combines them in
  // round-robin ordering.
  std::vector<Attribute> out_attrs(output_type.getNumElements());
  int64_t out = 0;
  for (int64_t outer = 0; outer < outer_size; ++outer) {
    for (auto op : operands) {
      const int64_t dim_size =
          op.getType().cast<RankedTensorType>().getDimSize(axis);
      const int64_t inner_size = dim_size * base_inner_size;

      auto input_attrs = op.cast<DenseElementsAttr>().getValues<Attribute>();
      auto input_iter = input_attrs.begin() + outer * inner_size;
      for (int64_t inner = 0; inner < inner_size; ++inner)
        out_attrs[out++] = *input_iter++;
    }
  }

  return DenseElementsAttr::get(output_type, out_attrs);
}

}  // end anonymous namespace

OpFoldResult ConcatenationOp::fold(ArrayRef<Attribute> operands) {
  if (fused_activation_function() == "NONE") {
    if (auto output_type = output()->getType().dyn_cast<RankedTensorType>()) {
      const int64_t axis = GetConcatenationOpAxis(*this);
      if (IsConcatenationOpConstFoldable(*this, operands, output_type, axis))
        return ConstFoldConcatenateOpDense(operands, output_type, axis);
    }
  }

  // Remove all empty values.
  SmallVector<Value *, 4> non_empty_values;
  for (Value *value : this->values()) {
    const auto shaped_type = value->getType().cast<ShapedType>();
    if (shaped_type.hasStaticShape() && shaped_type.getNumElements() == 0) {
      continue;
    }
    non_empty_values.push_back(value);
  }

  // All are not empty, do nothing.
  if (non_empty_values.size() == getNumOperands()) return nullptr;

  // If only one input is non-empty, just return it as the result of folding.
  if (non_empty_values.size() == 1) {
    return non_empty_values[0];
  }

  // Otherwise, build a new concatenation op with non-empty values.
  mlir::OpBuilder builder(getOperation());
  auto new_concat = builder.create<TFL::ConcatenationOp>(
      getLoc(), getType(), non_empty_values,
      builder.getIntegerAttr(builder.getIntegerType(32), axis()),
      builder.getStringAttr(fused_activation_function()));
  return new_concat.getResult();
}

//===----------------------------------------------------------------------===//
// FullyConnectedOp
//===----------------------------------------------------------------------===//

LogicalResult Verify(FullyConnectedOp op) {
  ShapedType input_type = op.input()->getType().cast<ShapedType>();
  ShapedType filter_type = op.filter()->getType().cast<ShapedType>();
  if (filter_type.hasRank() && filter_type.getRank() != 2) {
    return op.emitOpError("expect 2d filter, got ") << filter_type;
  }

  if (!input_type.hasStaticShape() || !filter_type.hasStaticShape()) {
    return mlir::success();
  }

  // Input's element size must be multiple of parameter's z_in dimension.
  const int z_in = filter_type.getDimSize(1);
  const int num_input_elements = input_type.getNumElements();
  if (num_input_elements % z_in != 0) {
    return op.emitOpError(llvm::formatv(
               "expect 'input' num_elements % {0} == 0, got input type ", z_in))
           << input_type;
  }

  // TODO(jpienaar): Include more shape verification for SHUFFLED4x16INT8
  // format.
  if (op.weights_format() == "DEFAULT") {
    ShapedType output_type =
        (*op.output().begin())->getType().cast<ShapedType>();
    if (!output_type.hasStaticShape()) {
      return mlir::success();
    }

    const int num_output_elements = output_type.getNumElements();
    const int z_out = filter_type.getDimSize(0);
    if (num_output_elements % z_out != 0) {
      return op.emitOpError(llvm::formatv(
                 "expect 'output' num_elements % {0} == 0, got ", z_out))
             << output_type;
    }

    if (num_input_elements / z_in != num_output_elements / z_out) {
      return op.emitOpError(
          "num_input_elements / z_in != num_output_elements / z_out");
    }
  }

  return mlir::success();
}

//===----------------------------------------------------------------------===//
// GatherOp
//===----------------------------------------------------------------------===//

static void BuildGatherOp(Builder *builder, OperationState &result,
                          Value *params, Value *indices, IntegerAttr axis) {
  auto params_type = params->getType().cast<TensorType>();
  auto indices_type = indices->getType().cast<TensorType>();

  // If params/indices is unranked, then output is unranked.
  if (!params_type.hasRank() || !indices_type.hasRank())
    return TFL::GatherOp::build(
        builder, result, builder->getTensorType(params_type.getElementType()),
        params, indices, axis);

  int64_t params_rank = params_type.getRank();
  int64_t indices_rank = indices_type.getRank();

  // params rank is guaranteed to be at least 1.
  // Produces an output tensor with shape:
  // params.shape[:axis] + indices.shape + params.shape[axis + 1:]
  std::vector<int64_t> shape(params_type.getShape());
  int64_t axis_i = axis.getInt();

  // For neg axis values, we wrap around params, e.g. axis = -1 => params[:-1]
  if (axis_i < 0) {
    axis_i += params_rank;
  }

  // params must be atleast rank axis + 1
  if (params_rank < axis_i + 1) {
    emitError(result.location, "params must be atleast rank axis + 1");
  }

  if (indices_rank == 0) {
    // Scalar indices (output is rank(params) - 1).
    // Erase shape[axis]
    shape.erase(shape.begin() + axis_i);
  } else if (indices_rank == 1) {
    // Vector indices (output is rank(params)).
    // Copy indices.shape into params.shape[axis]
    std::copy(std::begin(indices_type.getShape()),
              std::end(indices_type.getShape()), std::begin(shape) + axis_i);
  } else {
    // Higher rank indices (output is rank(params) + rank(indices) - 1).
    shape.resize(params_rank + indices_rank - 1);
    // Copy params.shape[axis + 1: ] into shape[axis + indices_rank:]
    std::copy(std::begin(params_type.getShape()) + axis_i + 1,
              std::end(params_type.getShape()),
              std::begin(shape) + axis_i + indices_rank);

    // Copy indices.shape into params.shape[axis]
    std::copy(std::begin(indices_type.getShape()),
              std::end(indices_type.getShape()), std::begin(shape) + axis_i);
  }

  TFL::GatherOp::build(
      builder, result,
      builder->getTensorType(shape, params_type.getElementType()), params,
      indices, axis);
}

//===----------------------------------------------------------------------===//
// MulOp
//===----------------------------------------------------------------------===//

OpFoldResult MulOp::fold(ArrayRef<Attribute> operands) {
  // TODO(b/142478136): Handle fused ops.
  if (fused_activation_function() != "NONE") return {};
  return ConstFoldBinaryOp(
      getType(), operands, [](APFloat a, APFloat b) { return a * b; },
      [](APInt a, APInt b) { return a * b; }, getOperation()->isCommutative());
}

//===----------------------------------------------------------------------===//
// DivOp
//===----------------------------------------------------------------------===//

OpFoldResult DivOp::fold(ArrayRef<Attribute> operands) {
  // TODO(b/142478136): Handle fused ops.
  if (fused_activation_function() != "NONE") return {};
  return ConstFoldBinaryOp(
      getType(), operands, [](APFloat a, APFloat b) { return a / b; },
      [](APInt a, APInt b) { return a.sdiv(b); },
      getOperation()->isCommutative());
}

//===----------------------------------------------------------------------===//
// PackOp
//===----------------------------------------------------------------------===//

// TODO(b/133486129): Implement shape inference for pack

static LogicalResult Verify(PackOp op) {
  // TODO(antiagainst): Implement other checks as in
  // tensorflow/lite/kernels/pack.cc

  if (op.getOperation()->getNumOperands() != op.values_count())
    return op.emitOpError("input count should match 'values_count' attribute");

  Value *operand0 = op.getOperand(0);
  auto input_type = operand0->getType().cast<ShapedType>();

  // Check axis bounds.
  if (input_type.hasRank()) {
    int64_t axis_value = op.axis().getSExtValue();
    if (abs(axis_value) > input_type.getRank())
      return op.emitOpError("op attribute 'axis' is out of bounds, got ")
             << axis_value;
  }

  // Make sure all inputs have the same shape and element type.
  // TODO(rahulsp): Simplify once b/135032064 is fixed.
  for (Value *operand : op.getOperands()) {
    auto other_type = operand->getType().cast<ShapedType>();
    if (input_type != other_type)
      return op.emitOpError("operands should be of the same type");
  }

  return success();
}

//===----------------------------------------------------------------------===//
// PReluOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(PReluOp op) {
  auto input_type = op.input()->getType().cast<ShapedType>();
  auto alpha_type = op.alpha()->getType().cast<ShapedType>();
  auto output_type = op.output()->getType().cast<ShapedType>();

  if (input_type.hasStaticShape() && alpha_type.hasStaticShape()) {
    if (input_type.getRank() != alpha_type.getRank() + 1) {
      return op.emitOpError("'alpha' should have one less rank than 'input'.");
    }

    // Check if alpha is broadcastable
    for (int i = 0; i < alpha_type.getRank(); i++) {
      if (alpha_type.getDimSize(i) != input_type.getDimSize(i + 1) &&
          alpha_type.getDimSize(i) != 1) {
        return op.emitOpError(
            llvm::formatv("'alpha' is not broadcastable at dimension {0}.", i));
      }
    }
  }

  if (input_type.hasStaticShape() && output_type.hasStaticShape()) {
    if (input_type.getRank() != output_type.getRank()) {
      return op.emitOpError("'input' and 'output' should have the same rank.");
    }

    // Check if input and output shapes are same
    for (int i = 0; i < input_type.getRank(); i++) {
      if (input_type.getDimSize(i) != output_type.getDimSize(i)) {
        return op.emitOpError(
            "'input' and 'output' should have the same shape.");
      }
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

namespace {
/// This pattern matches and merges a tfl.reshape under the following
/// condition:
/// * The input's defining op is another tfl.reshape.
// TODO(antiagainst): This pattern probably should be moved to the peephole
// category, after we have the infra for peephole passes.
struct RemoveAdjacentReshape : public RewritePattern {
  RemoveAdjacentReshape(MLIRContext *context)
      : RewritePattern(ReshapeOp::getOperationName(), 1, context) {}

  PatternMatchResult match(Operation *op) const override {
    auto thisOp = cast<ReshapeOp>(op);
    auto prevOp = thisOp.getOperand(0)->getDefiningOp();
    return isa_and_nonnull<ReshapeOp>(prevOp) ? matchSuccess() : matchFailure();
  }

  void rewrite(Operation *op, PatternRewriter &rewriter) const override {
    auto thisOp = cast<ReshapeOp>(op);
    auto prevOp = cast<ReshapeOp>(thisOp.getOperand(0)->getDefiningOp());

    // Replace
    //   %1 = "tfl.reshape"(%0, %shape0)
    //   %2 = "tfl.reshape"(%1, %shape1)
    // With
    //   %2 = "tfl.reshape"(%0, %shape1)
    rewriter.replaceOpWithNewOp<ReshapeOp>(
        {prevOp.getResult()}, op, thisOp.getType(), prevOp.getOperand(0),
        thisOp.getOperand(1));
  }
};

}  // end anonymous namespace

OpFoldResult ReshapeOp::fold(ArrayRef<Attribute> operands) {
  // Remove identity reshape with both static result and input shape.
  auto result_type = getType().cast<ShapedType>();
  auto input_type = getOperand(0)->getType().cast<ShapedType>();
  if (result_type.hasStaticShape() && result_type == input_type) {
    return getOperand(0);
  }

  // Constant folding
  if (auto dense_elements = operands[0].dyn_cast_or_null<DenseElementsAttr>()) {
    // If the result type isn't static, tries to derive the result type from
    // the #2 operand.
    if (!result_type.hasStaticShape()) {
      auto shape_elements = operands[1].dyn_cast_or_null<DenseElementsAttr>();
      if (!shape_elements) return nullptr;

      SmallVector<int64_t, 4> shape_data;
      for (auto it : shape_elements.getValues<APInt>()) {
        shape_data.push_back(it.getSExtValue());
      }
      result_type =
          RankedTensorType::get(shape_data, input_type.getElementType());
    }
    return dense_elements.reshape(result_type);
  }

  return nullptr;
}

void ReshapeOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  results.insert<RemoveAdjacentReshape>(context);
}

//===----------------------------------------------------------------------===//
// PackOp
//===----------------------------------------------------------------------===//

// Remove redundant unpack pack op.
// If a unpack op is followed by a pack op, we can remove the pack op, if the
// unpack op is only consumed by the pack op, it will be removed as well.
// An example illustration is:
//                  Unpack [5, 8, 9], axis = 1
//                /       \
//            value  ...  value [5, 9], 8 values in total
//              \           /
//                 pack,   axis = 1
//                   |
//               value   [5, 8, 9]
//
//   This can actually be simplified into just:
//
//           =>   Value [5, 8, 9]
// TODO(b/133341698): Move to tablegen when variadic is supported.
struct RemoveRedunantUnpackPack : public RewritePattern {
  explicit RemoveRedunantUnpackPack(MLIRContext *context)
      : RewritePattern(PackOp::getOperationName(), 2, context) {}

  PatternMatchResult matchAndRewrite(Operation *op,
                                     PatternRewriter &rewriter) const override {
    TFL::PackOp pack_op = cast<TFL::PackOp>(op);
    Operation *first_input = pack_op.getOperand(0)->getDefiningOp();
    if (!first_input) return matchFailure();
    auto input_unpack_op = dyn_cast_or_null<TFL::UnpackOp>(first_input);
    if (!input_unpack_op) return matchFailure();

    // The unpack & pack should have the same axis & num inputs/outputs.
    if (pack_op.axis() != input_unpack_op.axis() ||
        pack_op.values_count() != input_unpack_op.num())
      return matchFailure();

    const int total_pack_inputs = pack_op.getNumOperands();
    if (total_pack_inputs != input_unpack_op.getNumResults())
      return matchFailure();
    for (auto input_output :
         llvm::zip(pack_op.getOperands(), input_unpack_op.getResults())) {
      Value *pack_input = std::get<0>(input_output);
      Value *unpack_output = std::get<1>(input_output);
      // Make sure the ordering is the same for the pack op & unpack op.
      if (pack_input != unpack_output) return matchFailure();
    }

    // Replace the pack's output to the unpack's input.
    rewriter.replaceOp(pack_op, input_unpack_op.getOperand());
    // At this point, we don't manually remove the redundant pack op & unpack op
    // (we cannot actually), but trust the PatterRewriter to garbage collect
    // these two ops.
    return matchSuccess();
  }
};

void PackOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                         MLIRContext *context) {
  results.insert<RemoveRedunantUnpackPack>(context);
}

//===----------------------------------------------------------------------===//
// SliceOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(SliceOp op) {
  auto input_type = op.input()->getType().cast<ShapedType>();
  auto begin_type = op.begin()->getType().cast<ShapedType>();
  auto size_type = op.size()->getType().cast<ShapedType>();
  if (input_type.hasStaticShape() && begin_type.hasStaticShape() &&
      size_type.hasStaticShape()) {
    if (input_type.getRank() != begin_type.getNumElements()) {
      return op.emitError(
          "begin tensor elements size is not equal to input tensor rank");
    }

    if (input_type.getRank() != size_type.getNumElements()) {
      return op.emitError(
          "size tensor elements size is not equal to input tensor rank");
    }
  }

  DenseIntElementsAttr begin;
  if (matchPattern(op.begin(), m_Constant(&begin))) {
    int axis = 0;
    for (auto begin_i : llvm::enumerate(begin)) {
      if (begin_i.value().getSExtValue() < 0) {
        return op.emitError(
            llvm::formatv("begin[{0}] cannot be negative", axis));
      }
      axis++;
    }
  }

  DenseIntElementsAttr size;
  if (matchPattern(op.size(), m_Constant(&size))) {
    int axis = 0;
    for (auto size_i : llvm::enumerate(size)) {
      if (size_i.value().getSExtValue() < -1) {
        return op.emitError(
            llvm::formatv("size[{0}] cannot be negative other than -1", axis));
      }
      axis++;
    }
  }

  if (begin && size && input_type.hasStaticShape()) {
    const int input_rank = begin.getNumElements();
    for (uint64_t i = 0; i < input_rank; i++) {
      int begin_i =
          begin.getValue({i}).cast<IntegerAttr>().getValue().getSExtValue();
      int size_i =
          size.getValue({i}).cast<IntegerAttr>().getValue().getSExtValue();
      int dim_i = input_type.getShape()[i];
      if (begin_i > dim_i) {
        return op.emitOpError(llvm::formatv(
            "begin[{0}] cannot exceed dimension length: {1}", i, dim_i));
      }
      if (size_i >= 0 && begin_i + size_i > dim_i) {
        return op.emitError(llvm::formatv(
            "begin[{0}] + size[{0}] cannot exceed dimension length: {1}", i,
            dim_i));
      }
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// SubOp
//===----------------------------------------------------------------------===//

OpFoldResult SubOp::fold(ArrayRef<Attribute> operands) {
  // TODO(b/142478136): Handle fused ops.
  if (fused_activation_function() != "NONE") return {};
  return ConstFoldBinaryOp(
      getType(), operands, [](APFloat a, APFloat b) { return a - b; },
      [](APInt a, APInt b) { return a - b; }, getOperation()->isCommutative());
}

//===----------------------------------------------------------------------===//
// TopKOp
//===----------------------------------------------------------------------===//

static void BuildTopKOp(Builder *builder, OperationState &result, Value *input,
                        Value *k) {
  // Output size is only known if k is constant value. A negative dimension is
  // considered dynamic so use -1 here if k is not a constant value.
  int const_k = -1;
  ElementsAttr cst;
  if (matchPattern(k, m_Constant(&cst)))
    // These casts should all be valid due to how Tensor constants are stored.
    // TODO(jpienaar): This should use a helper function.
    const_k = cst.getValue<IntegerAttr>({}).getValue().getSExtValue();

  auto val_type = input->getType().cast<TensorType>();
  // If value is unranked, then so is results.
  if (!val_type.hasRank())
    return TFL::TopKV2Op::build(
        builder, result, builder->getTensorType(val_type.getElementType()),
        builder->getTensorType(builder->getIntegerType(32)), input, k);

  // Resultant shape is value.shape[:-1] + [k]
  std::vector<int64_t> shape(val_type.getShape());
  shape[shape.size() - 1] = const_k;
  TFL::TopKV2Op::build(
      builder, result, builder->getTensorType(shape, val_type.getElementType()),
      builder->getTensorType(shape, builder->getIntegerType(32)), input, k);
}

//===----------------------------------------------------------------------===//
// FakeQuantOp
//===----------------------------------------------------------------------===//

// Return true if the op has non-empty "minmax" attribute.
static inline bool HasValidMinMaxAttribute(Operation *op) {
  auto minmax = op->getAttrOfType<ArrayAttr>("minmax");
  return minmax && minmax.getValue().size() == 2;
}

namespace {

/// This pattern matches and remove a tfl.fake_quant if all the users of this op
/// and itself have "minmax" attribute set.
struct DropFakeQuant : public RewritePattern {
  explicit DropFakeQuant(MLIRContext *context)
      : RewritePattern(FakeQuantOp::getOperationName(), 1, context) {}

  PatternMatchResult match(Operation *op) const override {
    // We only match the op with valid "minmax" attribute.
    if (!HasValidMinMaxAttribute(op)) return matchFailure();

    // If all the users of this op have valid "minmax" attributes, it is matched
    // and can be removed.
    auto fakeQuantOp = cast<FakeQuantOp>(op);
    for (auto *operand : fakeQuantOp.getResult()->getUsers())
      if (!HasValidMinMaxAttribute(operand)) return matchFailure();

    return matchSuccess();
  }

  void rewrite(Operation *op, PatternRewriter &rewriter) const override {
    // Replace the matched FakeQuantOp by its primiary operand.
    rewriter.replaceOp(op, op->getOperand(0));
  }
};
}  // end anonymous namespace

void FakeQuantOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.insert<DropFakeQuant>(context);
}

//===----------------------------------------------------------------------===//
// UnpackOp
//===----------------------------------------------------------------------===//

// TODO(b/133486129): Implement shape inference for unpack

static LogicalResult Verify(UnpackOp op) {
  // TODO(antiagainst): Implement other checks as in
  // tensorflow/lite/kernels/unpack.cc

  if (op.getOperation()->getNumResults() != op.num())
    return op.emitOpError("output count should match 'num' attribute");

  return success();
}

//===----------------------------------------------------------------------===//
// SplitOp
//===----------------------------------------------------------------------===//

// Extracts and returns the signed integer constant in a 0-rank integer tensor
// if 'value' is a constant.
static llvm::Optional<int64_t> ExtractConstantIntFromTensor(Value *value) {
  ElementsAttr attr;
  if (!matchPattern(value, m_Constant(&attr))) return {};
  IntegerAttr int_attr = attr.getValue(llvm::None).cast<IntegerAttr>();
  return int_attr.getValue().getSExtValue();
}

// Returns a RankedTensorType which is similar to `input_type` but replaces the
// dimension size of `dim` with `dim_size`.  For example,
// `SubstituteRankedTensorTypeDimSize(tensor<3x4xi32>, 1, 2)` returns
// `tensor<3x2xi32>`.
static RankedTensorType SubstituteRankedTensorTypeDimSize(
    RankedTensorType input_type, int64_t dim, int64_t dim_size) {
  auto shape = input_type.getShape().vec();
  shape[dim] = dim_size;
  return RankedTensorType::get(shape, input_type.getElementType());
}

// Verifies the output tensor types of SplitOp or SplitVOp.
template <typename ExpectedOutputTypeGetter>
static LogicalResult VerifySplitOpOutputTypes(
    Operation *op, int64_t num_splits,
    ExpectedOutputTypeGetter get_expected_output_type) {
  for (int64_t i = 0; i < num_splits; ++i) {
    auto expected_output_type = get_expected_output_type(i);
    Value *output = op->getResult(i);
    auto output_type = output->getType().dyn_cast<RankedTensorType>();
    if (!output_type || output_type != expected_output_type)
      return op->emitOpError()
             << "output #" << i << " should be " << expected_output_type;
  }
  return success();
}

static LogicalResult Verify(SplitOp op) {
  int64_t num_splits = op.num_splits().getSExtValue();
  if (op.getNumResults() != num_splits)
    return op.emitOpError("output count should match 'num_splits' attribute");

  // If 'split_dim' is not a constant, there are no other checks.
  llvm::Optional<int64_t> split_dim_opt =
      ExtractConstantIntFromTensor(op.split_dim());
  if (!split_dim_opt) return success();

  // If 'input' is not a ranked tensor, there are no other checks.
  auto input_type = op.value()->getType().dyn_cast<RankedTensorType>();
  if (!input_type) return success();

  int64_t split_dim = split_dim_opt.getValue();
  const int64_t rank = input_type.getRank();
  if (split_dim < 0) split_dim += rank;
  if (split_dim < 0 || split_dim >= rank)
    return op.emitOpError("'split_dim' should be in [-rank, rank)");

  // If the 'split_dim' dimension of the 'input' tensor has a dynamic size,
  // there are no other checks.
  const int64_t dim_size = input_type.getDimSize(split_dim);
  if (ShapedType::isDynamic(dim_size)) return success();

  if (dim_size % num_splits != 0)
    return op.emitOpError("'num_splits' should evenly divide 'split_dim' axis");

  // Verifies output tensor types.
  RankedTensorType expected_output_type = SubstituteRankedTensorTypeDimSize(
      input_type, split_dim, dim_size / num_splits);
  return VerifySplitOpOutputTypes(
      op.getOperation(), num_splits,
      [expected_output_type](int64_t) { return expected_output_type; });
}

static LogicalResult Verify(SplitVOp op) {
  int64_t num_splits = op.num_splits().getSExtValue();
  if (op.getNumResults() != num_splits)
    return op.emitOpError("output count should match 'num_splits' attribute");

  // If 'split_dim' is not a constant, there are no other checks.
  llvm::Optional<int64_t> split_dim_opt =
      ExtractConstantIntFromTensor(op.split_dim());
  if (!split_dim_opt) return success();

  // If 'input' is not a ranked tensor, there are no other checks.
  auto input_type = op.value()->getType().dyn_cast<RankedTensorType>();
  if (!input_type) return success();

  int64_t split_dim = split_dim_opt.getValue();
  const int64_t rank = input_type.getRank();
  if (split_dim < 0) split_dim += rank;
  if (split_dim < 0 || split_dim >= rank)
    return op.emitOpError("'split_dim' should be in [-rank, rank)");

  // If the 'split_dim' dimension of the 'input' tensor has a dynamic size,
  // there are no other checks.
  const int64_t dim_size = input_type.getDimSize(split_dim);
  if (ShapedType::isDynamic(dim_size)) return success();

  // If 'size_splits' is not a constant, there are no other checks.
  ElementsAttr size_splits_attr;
  if (!matchPattern(op.size_splits(), m_Constant(&size_splits_attr)))
    return success();

  if (size_splits_attr.getNumElements() != num_splits) {
    auto size_splits_type =
        op.size_splits()->getType().cast<RankedTensorType>();
    RankedTensorType expected_size_splits_type =
        RankedTensorType::get({num_splits}, size_splits_type.getElementType());
    return op.emitOpError("'size_splits' should be ")
           << expected_size_splits_type;
  }

  // Normalizes and verifies 'size_splits'.
  // Note: TensorFlow allows one -1 element in 'size_splits'.  The -1 element
  // means the rest of the dimension size.
  llvm::SmallVector<int64_t, 4> size_splits;
  size_splits.reserve(num_splits);

  int64_t negative_size_split_loc = -1;
  int64_t total_size_splits = 0;

  for (int64_t i = 0; i < num_splits; ++i) {
    auto size_split_attr = size_splits_attr.getValue<IntegerAttr>(i);
    int64_t size_split = size_split_attr.getValue().getSExtValue();
    size_splits.push_back(size_split);
    if (size_split >= 0) {
      total_size_splits += size_split;
      continue;
    }
    if (size_split < -1)
      return op.emitOpError(
          "elements of 'size_splits' should be greater than or equal to -1");
    if (negative_size_split_loc != -1)
      return op.emitOpError("'size_splits' can only have one -1");
    negative_size_split_loc = i;
  }

  if (negative_size_split_loc != -1) {
    if (total_size_splits > dim_size)
      return op.emitOpError(
          "sum of non-negative elements of 'size_splits' is greater than the "
          "dimension size of 'split_dim' axis");
    size_splits[negative_size_split_loc] = dim_size - total_size_splits;
    total_size_splits = dim_size;
  }

  if (total_size_splits != dim_size)
    return op.emitOpError(
        "sum of 'size_splits' should match the dimension size of 'split_dim' "
        "axis");

  // Verifies result tensor types.
  auto get_expected_output_type = [input_type, split_dim,
                                   &size_splits](int64_t i) {
    return SubstituteRankedTensorTypeDimSize(input_type, split_dim,
                                             size_splits[i]);
  };
  return VerifySplitOpOutputTypes(op.getOperation(), num_splits,
                                  get_expected_output_type);
}

//===----------------------------------------------------------------------===//
// MeanOp
//===----------------------------------------------------------------------===//

// TODO(b/133854225): Implement shape inference to Mean

//===----------------------------------------------------------------------===//
// LSTMOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(LSTMOp op) {
  auto operands = op.GetStatefulOperands();
  if (operands.size() == 2 && operands[0] == 18 && operands[1] == 19) {
    return success();
  }
  return op.emitError("LSTMOp expected to have two stateful operands");
}

//===----------------------------------------------------------------------===//
// UnidirectionalSequenceLSTMOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(UnidirectionalSequenceLSTMOp op) {
  auto operands = op.GetStatefulOperands();
  if (operands.size() == 2 && operands[0] == 18 && operands[1] == 19) {
    return success();
  }
  return op.emitError(
      "UnidirectionalSequenceLSTMOp expected to have two stateful operands");
}

//===----------------------------------------------------------------------===//
// UnidirectionalSequenceRNNOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(UnidirectionalSequenceRNNOp op) {
  auto operands = op.GetStatefulOperands();
  if (operands.size() == 1 && operands[0] == 4) {
    return success();
  }
  return op.emitError(
      "UnidirectionalSequenceRNNOp expected to have one stateful operand");
}

//===----------------------------------------------------------------------===//
// SvdfOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(SVDFOp op) {
  auto operands = op.GetStatefulOperands();
  if (operands.size() == 1 && operands[0] == 4) {
    return success();
  }
  return op.emitError("SvdfOp expected to have one stateful operand");
}

//===----------------------------------------------------------------------===//
// AbsOp
//===----------------------------------------------------------------------===//

OpFoldResult AbsOp::fold(ArrayRef<Attribute> operands) {
  Type result_type = getType();
  // Only constant fold for tensor of f32 is implemented.
  if (!IsF32ShapedType(result_type)) return nullptr;

  auto compute = [](APFloat value) -> APFloat { return llvm::abs(value); };
  return ConstFoldUnaryOp(result_type, operands[0], compute);
}

//===----------------------------------------------------------------------===//
// SinOp
//===----------------------------------------------------------------------===//

OpFoldResult SinOp::fold(ArrayRef<Attribute> operands) {
  Type result_type = getType();
  // Only constant fold for tensor of f32 is implemented.
  if (!IsF32ShapedType(result_type)) return nullptr;

  auto compute = [](APFloat value) -> APFloat {
    float f = value.convertToFloat();
    float result = std::sin(f);
    return APFloat(result);
  };
  return ConstFoldUnaryOp(result_type, operands[0], compute);
}

//===----------------------------------------------------------------------===//
// CosOp
//===----------------------------------------------------------------------===//

OpFoldResult CosOp::fold(ArrayRef<Attribute> operands) {
  Type result_type = getType();
  // Only constant fold for tensor of f32 is implemented.
  if (!IsF32ShapedType(result_type)) return nullptr;

  auto compute = [](APFloat value) -> APFloat {
    float f = value.convertToFloat();
    float result = std::cos(f);
    return APFloat(result);
  };
  return ConstFoldUnaryOp(result_type, operands[0], compute);
}

//===----------------------------------------------------------------------===//
// LogOp
//===----------------------------------------------------------------------===//

OpFoldResult LogOp::fold(ArrayRef<Attribute> operands) {
  Type result_type = getType();
  // Only constant fold for tensor of f32 is implemented.
  if (!IsF32ShapedType(result_type)) return nullptr;

  auto compute = [](APFloat value) -> APFloat {
    float f = value.convertToFloat();
    float result = std::log(f);
    return APFloat(result);
  };
  return ConstFoldUnaryOp(result_type, operands[0], compute);
}

//===----------------------------------------------------------------------===//
// SqrtOp
//===----------------------------------------------------------------------===//

OpFoldResult SqrtOp::fold(ArrayRef<Attribute> operands) {
  Type result_type = getType();
  // Only constant fold for tensor of f32 is implemented.
  if (!IsF32ShapedType(result_type)) return nullptr;

  auto compute = [](APFloat value) -> APFloat {
    float f = value.convertToFloat();
    float result = std::sqrt(f);
    return APFloat(result);
  };
  return ConstFoldUnaryOp(result_type, operands[0], compute);
}

//===----------------------------------------------------------------------===//
// RsqrtOp
//===----------------------------------------------------------------------===//

OpFoldResult RsqrtOp::fold(ArrayRef<Attribute> operands) {
  Type result_type = getType();
  // Only constant fold for tensor of f32 is implemented.
  if (!IsF32ShapedType(result_type)) return nullptr;

  auto compute = [](APFloat value) -> APFloat {
    float f = value.convertToFloat();
    float result = 1.f / std::sqrt(f);
    return APFloat(result);
  };
  return ConstFoldUnaryOp(result_type, operands[0], compute);
}

//===----------------------------------------------------------------------===//
// SquareOp
//===----------------------------------------------------------------------===//

OpFoldResult SquareOp::fold(ArrayRef<Attribute> operands) {
  Type result_type = getType();
  // Only constant fold for tensor of f32 is implemented.
  if (!IsF32ShapedType(result_type)) return nullptr;

  auto compute = [](APFloat value) -> APFloat { return value * value; };
  return ConstFoldUnaryOp(result_type, operands[0], compute);
}

//===----------------------------------------------------------------------===//
// RankOp
//===----------------------------------------------------------------------===//

OpFoldResult RankOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.size() == 1);
  auto result_type = getType().cast<ShapedType>();
  if (auto elements_attr = operands[0].dyn_cast_or_null<ElementsAttr>()) {
    auto rank = static_cast<int32_t>(elements_attr.getType().getRank());
    return DenseElementsAttr::get(result_type, {rank});
  }

  // Also fold if `input` has a known rank.
  auto input_type = input()->getType().cast<ShapedType>();
  // Do not fold if rank is zero because the TFLite converter doesn't
  // distinguish between unranked input and scalar input due to b/138865275.
  // TODO(b/138865275): Remove `input_type.getRank() != 0` in the following
  // predicate and fold the op when rank is zero.
  if (input_type.hasRank() && input_type.getRank() != 0) {
    auto rank = static_cast<int32_t>(input_type.getRank());
    return DenseElementsAttr::get(result_type, {rank});
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// ConstOp
//===----------------------------------------------------------------------===//

OpFoldResult ConstOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.empty() && "constant has no operands");

  // Return the held attribute value.
  return value();
}

//===----------------------------------------------------------------------===//
// RangeOp
//===----------------------------------------------------------------------===//

namespace {

// Compute the length of a range (1-D) tensor given `start`, `limit`, `delta`.
// Template parameter `FloatOrInt` must be standard C integer or floating-point
// types.
template <typename FloatOrInt>
int GetLengthOfRange(FloatOrInt start, FloatOrInt limit, FloatOrInt delta) {
  // Refer to the implementation in
  // tensorflow/lite/kernels/range.cc.
  return std::is_integral<FloatOrInt>::value
             ? ((std::abs(limit - start) + std::abs(delta) - 1) /
                std::abs(delta))
             : std::ceil(std::abs((limit - start) / delta));
}

// Builds a constant range tensor of `result_elem_type` elements.
// Template parameter `FloatOrIntAtrr` must be mlir::IntegerAttr or
// mlir::FloatAttr.
template <typename FloatOrIntAtrr>
DenseElementsAttr BuildConstRangeTensor(Type result_elem_type, int num_elements,
                                        FloatOrIntAtrr start_attr,
                                        FloatOrIntAtrr delta_attr) {
  using ValueType = typename FloatOrIntAtrr::ValueType;  // APInt or APFloat
  ValueType start = start_attr.getValue();
  ValueType delta = delta_attr.getValue();

  SmallVector<ValueType, 16> new_values;
  new_values.reserve(num_elements);
  ValueType new_value = start;
  for (int i = 0; i < num_elements; ++i) {
    new_values.push_back(new_value);
    new_value = new_value + delta;
  }
  // Result is always a 1-D tensor.
  auto new_result_type =
      RankedTensorType::get({num_elements}, result_elem_type);
  return DenseElementsAttr::get(new_result_type, new_values);
}
}  // namespace

OpFoldResult RangeOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.size() == 3);
  auto start_tensor = operands[0].dyn_cast_or_null<ElementsAttr>();
  auto limit_tensor = operands[1].dyn_cast_or_null<ElementsAttr>();
  auto delta_tensor = operands[2].dyn_cast_or_null<ElementsAttr>();
  if (start_tensor && limit_tensor && delta_tensor) {
    // Operands should all be scalars
    assert(start_tensor.getType().getRank() == 0 &&
           limit_tensor.getType().getRank() == 0 &&
           delta_tensor.getType().getRank() == 0);
    Type elem_type = getType().cast<ShapedType>().getElementType();
    if (elem_type.isa<IntegerType>()) {
      auto start_attr = start_tensor.getValue<IntegerAttr>({});
      auto limit_attr = limit_tensor.getValue<IntegerAttr>({});
      auto delta_attr = delta_tensor.getValue<IntegerAttr>({});
      const int num_elements = GetLengthOfRange(
          start_attr.getInt(), limit_attr.getInt(), delta_attr.getInt());
      return BuildConstRangeTensor(elem_type, num_elements, start_attr,
                                   delta_attr);
    } else if (elem_type.isa<FloatType>()) {
      auto start_attr = start_tensor.getValue<FloatAttr>({});
      auto limit_attr = limit_tensor.getValue<FloatAttr>({});
      auto delta_attr = delta_tensor.getValue<FloatAttr>({});
      const int num_elements = GetLengthOfRange(start_attr.getValueAsDouble(),
                                                limit_attr.getValueAsDouble(),
                                                delta_attr.getValueAsDouble());
      return BuildConstRangeTensor(elem_type, num_elements, start_attr,
                                   delta_attr);
    }
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// TransposeConvOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(TransposeConvOp op) {
  ShapedType output_type = op.output()->getType().cast<ShapedType>();
  ShapedType output_shape_type =
      op.output_shape()->getType().cast<ShapedType>();
  if (output_type.hasRank() && output_shape_type.hasStaticShape()) {
    if (output_type.getRank() != output_shape_type.getDimSize(0)) {
      return op.emitOpError(llvm::formatv(
          "expect output type has rank = {0}, got output type {1}",
          output_shape_type.getDimSize(0), output_type));
    }
  }

  DenseIntElementsAttr output_shape_elements;
  if (!matchPattern(op.output_shape(), m_Constant(&output_shape_elements))) {
    return success();
  }

  llvm::SmallVector<int64_t, 4> output_shape;
  output_shape.reserve(output_shape_elements.getNumElements());
  for (auto dim : output_shape_elements.getValues<int>()) {
    output_shape.push_back(dim);
  }

  auto expected_output_type =
      RankedTensorType::get(output_shape, output_type.getElementType());
  if (output_type != expected_output_type) {
    return op.emitOpError(llvm::formatv("expect output type {0}, got {1}",
                                        expected_output_type, output_type));
  }

  return success();
}

//===----------------------------------------------------------------------===//
// TransposeOp
//===----------------------------------------------------------------------===//

namespace {

// Computes the permutation of a constant `input_tensor` according to `perm`.
// The function recursively traverses the dimensions of the output tensor in
// a row-major order and writes the value in the output tensor into
// `new_values`.
void ComputePermutation(ElementsAttr input_tensor, ArrayRef<int32_t> perm,
                        ArrayRef<int64_t> output_shape, int num_dimensions,
                        int output_axis, std::vector<uint64_t> *input_indices,
                        std::vector<Attribute> *new_values) {
  // Refer to the implementation of `Transpose` function in
  // tensorflow/lite/kernels/internal/reference/reference_ops.h
  assert(output_axis < num_dimensions);
  const int input_axis = perm[output_axis];
  for (int i = 0; i < output_shape[output_axis]; ++i) {
    // Update the input indices on `input_axis`.
    input_indices->at(input_axis) = i;
    // Write the value from `input_tensor` if it is the last axis or
    // recurse into the next axis.
    const bool is_last_axis = output_axis == num_dimensions - 1;
    if (is_last_axis) {
      new_values->push_back(input_tensor.getValue(*input_indices));
    } else {
      ComputePermutation(input_tensor, perm, output_shape, num_dimensions,
                         output_axis + 1, input_indices, new_values);
    }
  }
}

}  // namespace

OpFoldResult TransposeOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.size() == 2);
  auto input_tensor = operands[0].dyn_cast_or_null<ElementsAttr>();
  auto perm_tensor = operands[1].dyn_cast_or_null<ElementsAttr>();
  if (!input_tensor || !perm_tensor) return nullptr;

  // Do not try to fold elements attr of a quant type because
  // DenseElementsAttr does not support it.
  if (!getType().cast<ShapedType>().getElementType().isIntOrFloat())
    return nullptr;

  assert(perm_tensor.getType().getRank() == 1);
  const int num_dimensions = input_tensor.getType().getRank();
  assert(perm_tensor.getType().getNumElements() == num_dimensions);

  ArrayRef<int64_t> input_shape = input_tensor.getType().getShape();
  auto output_type = getType().cast<ShapedType>();

  SmallVector<int32_t, 4> perm;
  SmallVector<int64_t, 4> output_shape;
  for (int i = 0; i < num_dimensions; ++i) {
    perm.push_back(
        perm_tensor.getValue<IntegerAttr>({static_cast<uint64_t>(i)}).getInt());
    output_shape.push_back(input_shape[perm[i]]);

    // Check that the derived output shape matches the static shape.
    assert(!output_type.hasStaticShape() ||
           output_type.getShape()[i] == output_shape[i]);
  }

  std::vector<Attribute> new_values;
  new_values.reserve(input_tensor.getType().getNumElements());
  std::vector<uint64_t> input_indices(num_dimensions);
  ComputePermutation(input_tensor, perm, output_shape, num_dimensions,
                     /*output_axis=*/0, &input_indices, &new_values);
  auto result_type =
      RankedTensorType::get(output_shape, output_type.getElementType());
  return DenseElementsAttr::get(result_type, new_values);
}

static LogicalResult Verify(TransposeOp op) {
  auto input_type = op.x()->getType().cast<ShapedType>();
  auto perm_type = op.perm()->getType().cast<ShapedType>();
  auto output_type = op.y()->getType().cast<ShapedType>();
  if (input_type.hasStaticShape() && perm_type.hasStaticShape()) {
    if (perm_type.getNumElements() != input_type.getRank()) {
      return op.emitOpError(
          "perm tensor elements size is not equal to input tensor rank");
    }
  }

  DenseIntElementsAttr perm;
  if (!matchPattern(op.perm(), m_Constant(&perm))) {
    return success();
  }

  int index = 0;
  llvm::SmallVector<int64_t, 4> axes;
  for (auto axis_int : perm.getValues<APInt>()) {
    const int64_t axis = axis_int.getSExtValue();
    if (axis < 0 || (input_type.hasRank() && axis >= input_type.getRank())) {
      return op.emitOpError(
          llvm::formatv("perm[{0}] must be in [0, rank)", index));
    }
    if (std::count(axes.begin(), axes.end(), axis) > 0) {
      return op.emitOpError(
          llvm::formatv("perm[{0}] cannot have duplicated axis", index));
    }
    axes.push_back(axis);
    index++;
  }

  if (input_type.hasStaticShape() && output_type.hasStaticShape()) {
    llvm::SmallVector<int64_t, 4> transposed_shape;
    for (int64_t axis : axes) {
      transposed_shape.push_back(input_type.getDimSize(axis));
    }
    auto expected_output_type =
        RankedTensorType::get(transposed_shape, input_type.getElementType());
    if (output_type != expected_output_type) {
      return op.emitOpError(llvm::formatv("expect output type {0}, got {1}",
                                          expected_output_type, output_type));
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.cc.inc"

Operation *TensorFlowLiteDialect::materializeConstant(OpBuilder &builder,
                                                      Attribute value,
                                                      Type type, Location loc) {
  // If this is an opaque elements attribute or the result type doesn't match
  // the attribute type, then generate a tfl.pseudo_const.
  if (value.isa<OpaqueElementsAttr>() ||
      (value.isa<ElementsAttr>() && value.getType() != type))
    return builder.create<ConstOp>(loc, type, value.cast<ElementsAttr>());
  return nullptr;
}

}  // namespace TFL
}  // namespace mlir
