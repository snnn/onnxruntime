// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "core/graph/contrib_ops/contrib_defs.h"

#include <cmath>
#if !defined(ORT_MINIMAL_BUILD)
#include "onnx/defs/schema.h"
#else
#include "onnx/defs/data_type_utils.h"
#endif
#include "onnx/defs/shape_inference.h"
#include "onnx/defs/tensor_proto_util.h"

#include "core/framework/tensorprotoutils.h"
#include "core/graph/constants.h"
#include "core/graph/contrib_ops/attn_lstm_schema_defs.h"
#include "core/graph/contrib_ops/range_schema_defs.h"
#include "core/graph/op.h"
#include "core/mlas/inc/mlas.h"
#include "core/graph/signal_ops/signal_defs.h"
#include "core/graph/contrib_ops/onnx_function_util.h"
#include "onnx/defs/function.h"

namespace ONNX_NAMESPACE {
void convPoolShapeInference(
    ONNX_NAMESPACE::InferenceContext& ctx,
    bool use_dilation, bool require_kernel_shape,
    int input1Idx,
    int input2Idx);
void matmulShapeInference(
    ONNX_NAMESPACE::InferenceContext& ctx,
    int input1Idx,
    int input2Idx);

void convTransposeWithDynamicPadsShapeInference(InferenceContext& ctx) {
  propagateElemTypeFromInputToOutput(ctx, 0, 0);

  // we need at least two inputs to have a shape for this inference.
  if (!hasNInputShapes(ctx, 2)) {
    return;
  }

  int64_t group = getAttribute(ctx, "group", 1);

  auto input_shape = ctx.getInputType(0)->tensor_type().shape();
  if (input_shape.dim_size() < 2) {
    return;  // Input tensor should have at least two dimensions.
  }

  // first dim is the batch axis and the next is the number of channels.
  size_t n_input_dims = static_cast<size_t>(input_shape.dim_size() - size_t{2});

  std::vector<int64_t> dilations;
  if (getRepeatedAttribute(ctx, "dilations", dilations)) {
    if (dilations.size() != n_input_dims) {
      return;
    }
  } else {
    dilations.assign(n_input_dims, 1);
  }

  std::vector<int64_t> strides;
  if (getRepeatedAttribute(ctx, "strides", strides)) {
    if (strides.size() != n_input_dims) {
      return;
    }
  } else {
    strides.assign(n_input_dims, 1);
  }

  std::vector<int64_t> kernel_shape;
  if (getRepeatedAttribute(ctx, "kernel_shape", kernel_shape)) {
    if (kernel_shape.size() != n_input_dims) {
      return;
    }
  } else {
    auto second_input_shape = ctx.getInputType(1)->tensor_type().shape();
    for (int i = 2; i < second_input_shape.dim_size(); ++i) {
      if (!second_input_shape.dim(i).has_dim_value()) {
        return;
      }
      kernel_shape.push_back(second_input_shape.dim(i).dim_value());
    }
  }

  std::vector<int64_t> effective_kernel_shape = kernel_shape;
  for (int i = 0; i < static_cast<int>(kernel_shape.size()); i++) {
    // accounting for dilation, how big is the kernel in this dimension
    effective_kernel_shape[i] =
        (effective_kernel_shape[i] - 1) * dilations[i] + 1;
  }

  std::vector<int64_t> pads;

  // Infer output shape if 'pads' tensor is available
  const auto* pads_initializer = ctx.getInputData(2);
  if (nullptr == pads_initializer) {
    return;
  }

  if (pads_initializer->dims_size() != 1 ||
      pads_initializer->data_type() != TensorProto::INT64)
    fail_shape_inference(
        "'pads' input must be a 1D (shape: [2 * n_input_dims]) tensor of type int64");

  pads = ParseData<int64_t>(pads_initializer);

  if (pads.size() != static_cast<size_t>(2 * n_input_dims))
    fail_shape_inference("Pads has incorrect number of values");

  std::vector<int64_t> output_shape;
  bool output_shape_presented = true;
  if (getRepeatedAttribute(ctx, "output_shape", output_shape)) {
    if (output_shape.size() != n_input_dims) {
      return;
    }
  } else {
    output_shape_presented = false;
  }

  std::vector<int64_t> output_padding;
  if (getRepeatedAttribute(ctx, "output_padding", output_padding)) {
    if (output_padding.size() != n_input_dims) {  // Added only to one side.
      return;
    }
  } else {
    output_padding.assign(n_input_dims, 0);
  }

  auto final_output_shape =
      ctx.getOutputType(0)->mutable_tensor_type()->mutable_shape();

  *final_output_shape->add_dim() = input_shape.dim(0);
  *final_output_shape->add_dim() =
      ctx.getInputType(1)->tensor_type().shape().dim(1) *
      group;  // channels should be the second dim of second input multiply
              // group.

  int size_of_output;
  if (output_shape_presented) {
    size_of_output = static_cast<int>(output_shape.size());
    for (int i = 0; i < size_of_output; ++i) {
      if (input_shape.dim(i + 2).has_dim_value()) {
        if (output_shape[i] < input_shape.dim(i + 2).dim_value()) {
          // TODO: throw exception?
          return;  // output shape value cannot be smaller than the input shape
                   // value
        }
      }
      final_output_shape->add_dim()->set_dim_value(output_shape[i]);
    }
    return;
  } else {
    size_of_output = input_shape.dim_size() - 2;
    for (int i = 0; i < size_of_output; ++i) {
      if (input_shape.dim(i + 2).has_dim_value()) {
        int64_t output_shape_dim =
            strides[i] * (input_shape.dim(i + 2).dim_value() - 1) +
            output_padding[i] + effective_kernel_shape[i] - pads[i] -
            pads[i + n_input_dims];
        final_output_shape->add_dim()->set_dim_value(output_shape_dim);
      } else {
        final_output_shape->add_dim();
      }
    }
    return;
  }
}

void embedLayerNormalizationShapeInference(InferenceContext& ctx) {
  propagateElemTypeFromInputToOutput(ctx, 2, 0);
  propagateElemTypeFromInputToOutput(ctx, 0, 1);
  if (!hasInputShape(ctx, 0)) {
    // TODO(kreeger): In this case update the output to (?, ?, hidden_size).
    return;
  }

  auto& input_ids_shape = getInputShape(ctx, 0);
  auto& input_ids_dims = input_ids_shape.dim();

  // Note that both batch size and sequence length could be symbolic.
  // So we only check dimension size here.
  if (input_ids_dims.size() != 2) {
    fail_shape_inference("input_ids shall be 2 dimensions");
  }

  bool has_segment = hasInputShape(ctx, 1);
  if (has_segment) {
    // Ensure that segment_ids has the same shape.
    auto& segment_ids_shape = getInputShape(ctx, 1);
    auto& segment_ids_dims = segment_ids_shape.dim();
    if (segment_ids_dims.size() != 2) {
      fail_shape_inference("segment_ids input shall be 2 dimensions");
    }
  }

  // get hidden_size from the last dimension of embedding
  auto& word_embedding_shape = getInputShape(ctx, 2);
  auto& word_embedding_dims = word_embedding_shape.dim();
  if (word_embedding_dims.size() != 2 ||
      !word_embedding_dims[1].has_dim_value() ||
      word_embedding_shape.dim(1).dim_value() <= 0) {
    fail_shape_inference("word_embedding should have 2 dimensions and dimension size is known.");
  }
  int64_t hidden_size = word_embedding_shape.dim(1).dim_value();

  // Ensure that all embeddings + the gamma/beta tensors have the same hidden_size:
  auto& position_embedding_shape = getInputShape(ctx, 3);
  auto& position_embedding_dims = position_embedding_shape.dim();
  if (position_embedding_dims.size() != 2 ||
      !position_embedding_dims[1].has_dim_value() ||
      position_embedding_shape.dim(1).dim_value() != hidden_size) {
    fail_shape_inference(
        "position_embedding should have 2 dimensions, dimension size known, "
        "and same hidden size as word_embedding.");
  }

  if (has_segment) {
    auto& segment_embedding_shape = getInputShape(ctx, 4);
    auto& segment_embedding_dims = segment_embedding_shape.dim();
    if (segment_embedding_dims.size() != 2 ||
        !segment_embedding_dims[1].has_dim_value() ||
        segment_embedding_shape.dim(1).dim_value() != hidden_size) {
      fail_shape_inference(
          "segment_embedding should have 2 dimensions, dimension size known, "
          "and same hidden size as word_embedding.");
    }
  }

  auto& gamma_shape = getInputShape(ctx, 5);
  auto& gamma_dims = gamma_shape.dim();
  if (gamma_dims.size() != 1 ||
      !gamma_dims[0].has_dim_value() ||
      gamma_shape.dim(0).dim_value() != hidden_size) {
    fail_shape_inference(
        "gamma should have 2 dimension, dimension size known, "
        "and same hidden size as word_embedding.");
  }

  auto& beta_shape = getInputShape(ctx, 6);
  auto& beta_dims = gamma_shape.dim();
  if (beta_dims.size() != 1 ||
      !beta_dims[0].has_dim_value() ||
      beta_shape.dim(0).dim_value() != hidden_size) {
    fail_shape_inference(
        "beta should have 1 dimension, dimension size known, "
        "and same hidden size as word_embedding.");
  }

  // input shape is (batch_size, sequence_length), output shape is (batch_size, sequence_length, hidden_size)
  ONNX_NAMESPACE::TensorShapeProto output_shape;
  *output_shape.add_dim() = input_ids_dims[0];
  *output_shape.add_dim() = input_ids_dims[1];

  output_shape.add_dim();
  output_shape.mutable_dim(2)->set_dim_value(hidden_size);

  updateOutputShape(ctx, 0, output_shape);

  // mask_index shape is (batch_size)
  ONNX_NAMESPACE::TensorShapeProto mask_index_shape;
  *mask_index_shape.add_dim() = input_ids_dims[0];
  updateOutputShape(ctx, 1, mask_index_shape);

  if (ctx.getNumOutputs() > 2) {
    updateOutputShape(ctx, 2, output_shape);
    propagateElemTypeFromInputToOutput(ctx, 0, 2);
  }
}
}  // namespace ONNX_NAMESPACE

namespace onnxruntime {
namespace contrib {
using namespace ONNX_NAMESPACE;
using ONNX_NAMESPACE::AttributeProto;
using ONNX_NAMESPACE::OpSchema;
using ONNX_NAMESPACE::OPTIONAL_VALUE;

void FusedMatMulShapeInference(ONNX_NAMESPACE::InferenceContext& ctx) {
  propagateElemTypeFromInputToOutput(ctx, 0, 0);
  auto transAAttr = ctx.getAttribute("transA");
  bool transa = transAAttr ? static_cast<int>(transAAttr->i()) != 0 : false;
  auto transBAttr = ctx.getAttribute("transB");
  bool transb = transBAttr ? static_cast<int>(transBAttr->i()) != 0 : false;
  auto trans_batch_a_attr = ctx.getAttribute("transBatchA");
  bool trans_batch_a = trans_batch_a_attr ? static_cast<int>(trans_batch_a_attr->i()) != 0 : false;
  auto trans_batch_b_attr = ctx.getAttribute("transBatchB");
  bool trans_batch_b = trans_batch_b_attr ? static_cast<int>(trans_batch_b_attr->i()) != 0 : false;
  int input1Idx = 0;
  int input2Idx = 1;
  if (!hasInputShape(ctx, input1Idx) || !hasInputShape(ctx, input2Idx)) {
    return;
  }

  const auto shape0_raw = getInputShape(ctx, input1Idx);
  const auto shape1_raw = getInputShape(ctx, input2Idx);

  if (shape0_raw.dim_size() == 0 || shape1_raw.dim_size() == 0) {
    fail_shape_inference("Input tensors of wrong rank (0).");
  }

  // numpy transpose on a vector does not change anything.
  if (shape0_raw.dim_size() == 1) {
    transa = false;
  }
  if (shape1_raw.dim_size() == 1) {
    transb = false;
  }

  ONNX_NAMESPACE::TensorShapeProto shape0, shape1;
  auto rank0 = shape0_raw.dim_size();
  if (rank0 == 1) {
    // for vector input, transa does not make impact on the dim.
    shape0 = shape0_raw;
  } else {
    int start = trans_batch_a ? 1 : 0;
    int end = trans_batch_a ? rank0 - 1 : rank0 - 2;
    for (int i = start; i < end; ++i) {
      *shape0.add_dim() = shape0_raw.dim(i);
    }
    *shape0.add_dim() = shape0_raw.dim(transa ? rank0 - 1 : (trans_batch_a ? 0 : rank0 - 2));
    *shape0.add_dim() = shape0_raw.dim(transa ? (trans_batch_a ? 0 : rank0 - 2) : rank0 - 1);
  }

  auto rank1 = shape1_raw.dim_size();
  if (rank1 == 1) {
    // for vector input, transb does not make impact on the dim.
    shape1 = shape1_raw;
  } else {
    int start = trans_batch_b ? 1 : 0;
    int end = trans_batch_b ? rank1 - 1 : rank1 - 2;
    for (int i = start; i < end; ++i) {
      *shape1.add_dim() = shape1_raw.dim(i);
    }
    *shape1.add_dim() = shape1_raw.dim(transb ? rank1 - 1 : (trans_batch_b ? 0 : rank1 - 2));
    *shape1.add_dim() = shape1_raw.dim(transb ? (trans_batch_b ? 0 : rank1 - 2) : rank1 - 1);
  }

  ONNX_NAMESPACE::TensorShapeProto shapeL, shapeR;

  // First promote each shape to at least rank-2. This logic is
  // specific to matmul, not generic broadcasting.
  {
    if (shape0.dim_size() == 1) {
      shapeL.add_dim()->set_dim_value(1);
      *shapeL.add_dim() = shape0.dim(0);
    } else {
      *shapeL.mutable_dim() = shape0.dim();
    }
    if (shape1.dim_size() == 1) {
      *shapeR.add_dim() = shape1.dim(0);
      shapeR.add_dim()->set_dim_value(1);
    } else {
      *shapeR.mutable_dim() = shape1.dim();
    }
  }

  // Check for compatible matrix multiply dimensions
  {
    auto dimL = shapeL.dim(shapeL.dim_size() - 1);
    auto dimR = shapeR.dim(shapeR.dim_size() - 2);
    if (dimL.has_dim_value() && dimR.has_dim_value() &&
        dimL.dim_value() != dimR.dim_value()) {
      fail_shape_inference("Incompatible dimensions for matrix multiplication");
    }
  }

  ONNX_NAMESPACE::TensorShapeProto resultShape;

  // Now call out to generic multidimensional broadcasting for
  // the broadcastable prefixes.
  {
    ONNX_NAMESPACE::TensorShapeProto prefixShapeL, prefixShapeR;
    for (int i = 0; i < shapeL.dim_size() - 2; ++i) {
      *prefixShapeL.add_dim() = shapeL.dim(i);
    }
    for (int i = 0; i < shapeR.dim_size() - 2; ++i) {
      *prefixShapeR.add_dim() = shapeR.dim(i);
    }
    bidirectionalBroadcastShapeInference(
        prefixShapeL, prefixShapeR, resultShape);
  }

  // Back to matmul-specific. Add the trailing dimensions back in.
  {
    if (shape0.dim_size() != 1) {
      *resultShape.add_dim() = shapeL.dim(shapeL.dim_size() - 2);
    }
    if (shape1.dim_size() != 1) {
      *resultShape.add_dim() = shapeR.dim(shapeR.dim_size() - 1);
    }
  }
  updateOutputShape(ctx, 0, resultShape);
}

// input1Idx - sparse matrix
// input2Idx - dense matrix.
// Output is dense
void sparseCompatibleMatmulShapeInference(
    ONNX_NAMESPACE::InferenceContext& ctx,
    int input1Idx,
    int input2Idx) {
  if (!hasInputShape(ctx, input1Idx) || !hasInputShape(ctx, input2Idx)) {
    return;
  }

  const auto shape0 = getInputShape(ctx, input1Idx);
  const auto shape1 = getInputShape(ctx, input2Idx);

  if (shape0.dim_size() == 0 || shape1.dim_size() == 0) {
    fail_shape_inference("Input tensors of wrong rank (0).");
  }

  ONNX_NAMESPACE::TensorShapeProto shapeL, shapeR;

  // First promote each shape to at least rank-2. This logic is
  // specific to matmul, not generic broadcasting.
  {
    if (shape0.dim_size() == 1) {
      shapeL.add_dim()->set_dim_value(1);
      *shapeL.add_dim() = shape0.dim(0);
    } else {
      *shapeL.mutable_dim() = shape0.dim();
    }
    if (shape1.dim_size() == 1) {
      *shapeR.add_dim() = shape1.dim(0);
      shapeR.add_dim()->set_dim_value(1);
    } else {
      *shapeR.mutable_dim() = shape1.dim();
    }
  }

  // Check for compatible matrix multiply dimensions
  {
    auto dimL = shapeL.dim(shapeL.dim_size() - 1);
    auto dimR = shapeR.dim(shapeR.dim_size() - 2);
    if (dimL.has_dim_value() && dimR.has_dim_value() &&
        dimL.dim_value() != dimR.dim_value()) {
      fail_shape_inference("Incompatible dimensions for matrix multiplication");
    }
  }

  ONNX_NAMESPACE::TensorShapeProto resultShape;

  // Now call out to generic multidimensional broadcasting for
  // the broadcastable prefixes.
  {
    ONNX_NAMESPACE::TensorShapeProto prefixShapeL, prefixShapeR;
    for (int i = 0; i < shapeL.dim_size() - 2; ++i) {
      *prefixShapeL.add_dim() = shapeL.dim(i);
    }
    for (int i = 0; i < shapeR.dim_size() - 2; ++i) {
      *prefixShapeR.add_dim() = shapeR.dim(i);
    }
    bidirectionalBroadcastShapeInference(
        prefixShapeL, prefixShapeR, resultShape);
  }

  // Back to matmul-specific. Add the trailing dimensions back in.
  {
    if (shape0.dim_size() != 1) {
      *resultShape.add_dim() = shapeL.dim(shapeL.dim_size() - 2);
    }
    if (shape1.dim_size() != 1) {
      *resultShape.add_dim() = shapeR.dim(shapeR.dim_size() - 1);
    }
  }

  // if the input 2 type was not previously propagate to output
  // we want to make sure that it is the tensor type of input 2
  auto default_tensor_type = ctx.getInputType(input2Idx)->value_case();
  updateOutputShape(ctx, 0, resultShape, default_tensor_type);
}

void AttentionTypeAndShapeInference(ONNX_NAMESPACE::InferenceContext& ctx, int past_input_index) {
  // Type inference
  ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 2, 0);
  if (ctx.getNumOutputs() > 1) {
    ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 2, 1);
  }

  // Shape inference
  if (hasInputShape(ctx, 0) && hasInputShape(ctx, 2)) {
    auto& input_shape = getInputShape(ctx, 0);
    auto& input_dims = input_shape.dim();
    if (input_dims.size() != 3) {
      fail_shape_inference("Inputs 0 shall be 3 dimensions");
    }

    auto& bias_shape = getInputShape(ctx, 2);
    auto& bias_dims = bias_shape.dim();
    if (bias_dims.size() != 1) {
      fail_shape_inference("Invalid bias shape");
    }

    std::vector<int64_t> qkv_hidden_sizes;
    getRepeatedAttribute(ctx, "qkv_hidden_sizes", qkv_hidden_sizes);

    int64_t output_hidden_size;
    if (qkv_hidden_sizes.size() != 0) {
      if (qkv_hidden_sizes.size() != 3) {
        fail_shape_inference("qkv_hidden_sizes should have 3 elements")
      }
      output_hidden_size = qkv_hidden_sizes[2];
    } else {
      output_hidden_size = bias_shape.dim(0).dim_value() / 3;
    }

    ONNX_NAMESPACE::TensorShapeProto output_shape;
    for (auto& dim : input_dims) {
      *output_shape.add_dim() = dim;
    }

    output_shape.mutable_dim(2)->set_dim_value(output_hidden_size);
    updateOutputShape(ctx, 0, output_shape);

    // TODO does the extra output need any changes?
    if (ctx.getNumOutputs() > 1) {
      if (hasInputShape(ctx, past_input_index)) {
        auto& past_shape = getInputShape(ctx, past_input_index);
        auto& past_dims = past_shape.dim();
        if (past_dims.size() != 5) {
          fail_shape_inference("Inputs 4 shall be 5 dimensions");
        }

        if (past_dims[3].has_dim_value() && input_dims[1].has_dim_value()) {
          auto all_sequence_length = past_shape.dim(3).dim_value() + input_shape.dim(1).dim_value();

          ONNX_NAMESPACE::TensorShapeProto present_shape;
          for (auto& dim : past_dims) {
            *present_shape.add_dim() = dim;
          }
          present_shape.mutable_dim(3)->set_dim_value(all_sequence_length);

          updateOutputShape(ctx, 1, present_shape);
        }
      }
    }
  }
}

void DecoderAttentionTypeAndShapeInference(ONNX_NAMESPACE::InferenceContext& ctx) {
  // Type inference
  ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 0);
  if (ctx.getNumOutputs() > 1) {
    ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 1);
    ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 2);
  }
  // Shape inference
  if (hasInputShape(ctx, 0)) {
    auto& query_shape = getInputShape(ctx, 0);
    updateOutputShape(ctx, 0, query_shape);
  }
  if (ctx.getNumOutputs() > 1) {
    if (hasInputShape(ctx, 6) && hasInputShape(ctx, 7)) {
      auto& cache_shape = getInputShape(ctx, 6);
      auto& cache_dims = cache_shape.dim();
      if (cache_dims.size() != 4) {
        fail_shape_inference("key and value cache shall be 4 dimensions");
      }
      // has_dim_value() will return false if value is dynamic
      if (cache_dims[0].has_dim_value() &&
          cache_dims[1].has_dim_value() &&
          cache_dims[2].has_dim_value() &&
          cache_dims[3].has_dim_value()) {

        ONNX_NAMESPACE::TensorShapeProto new_cache_shape;
        *new_cache_shape.add_dim() = cache_shape.dim(0);
        *new_cache_shape.add_dim() = cache_shape.dim(1);
        new_cache_shape.add_dim();
        *new_cache_shape.add_dim() = cache_shape.dim(3);

        updateOutputShape(ctx, 1, new_cache_shape);
        updateOutputShape(ctx, 2, new_cache_shape);
      }
    }
  }
}

bool ParseScalar(const TensorProto* initializer, int& value) {
  std::vector<int32_t> parsed_data;
  if (initializer->data_type() == TensorProto::INT32) {
    const auto& data = ParseData<int32_t>(initializer);
    parsed_data.insert(parsed_data.end(), data.begin(), data.end());

    if (parsed_data.size() == 1) {
      value = parsed_data[0];
      return true;
    }
  }

  return false;
}

void BeamSearchShapeInference(ONNX_NAMESPACE::InferenceContext& ctx) {
  // Type inference
  ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 0);

  if (ctx.getNumOutputs() > 1) {
    // Here we assume that the third output exist only if second output exists.
    ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 5, 1);
    if (ctx.getNumOutputs() > 2) {
      ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 5, 2);
    }
  }

  // Shape inference
  // input 0 (input_ids) shape: (batch_size, sequence_length)
  // output 0 (sequences) shape: (batch_size, num_return_sequences, max_length)
  // output 1 (sequences_scores) shape: (batch_size, num_return_sequences)
  // output 2 (scores) shape: (max_length - sequence_length, batch_size, num_beams, vocab_size)
  if (!hasInputShape(ctx, 0)) {
    return;
  }
  auto& input_ids_shape = getInputShape(ctx, 0);
  auto& input_ids_dims = input_ids_shape.dim();
  if (input_ids_dims.size() != 2) {
    fail_shape_inference("Inputs 0 shall be 2 dimensions");
  }
  if (!(input_ids_dims[0].has_dim_value() && input_ids_dims[1].has_dim_value())) {
    return;
  }

  int64_t batch_size = input_ids_dims[0].dim_value();
  int64_t sequence_length = input_ids_dims[1].dim_value();

  const auto max_length = ctx.getInputData(1);
  const auto num_beams = ctx.getInputData(3);
  const auto num_return_sequences = ctx.getInputData(4);
  if (num_beams == nullptr || max_length == nullptr || num_return_sequences == nullptr) {  // not initializer
    return;
  }

  int max_length_value = 0;
  if (!ParseScalar(max_length, max_length_value) || max_length_value <= 0) {
    fail_shape_inference("Failed to parse max_length or it is not positive integer scalar");
  }

  int num_beams_value = 0;
  if (!ParseScalar(num_beams, num_beams_value) || num_beams_value <= 0) {
    fail_shape_inference("Failed to parse num_beams or it is not positive integer scalar");
  }

  int num_return_sequences_value = 0;
  if (!ParseScalar(num_return_sequences, num_return_sequences_value) || num_return_sequences_value <= 0) {
    fail_shape_inference("Failed to parse num_return_sequences or it is not positive integer scalar");
  }

  ONNX_NAMESPACE::TensorShapeProto sequences_shape;
  sequences_shape.add_dim()->set_dim_value(batch_size);
  sequences_shape.add_dim()->set_dim_value(num_return_sequences_value);
  sequences_shape.add_dim()->set_dim_value(max_length_value);
  updateOutputShape(ctx, 0, sequences_shape);

  if (ctx.getNumOutputs() > 1) {
    ONNX_NAMESPACE::TensorShapeProto sequences_scores_shape;
    sequences_shape.add_dim()->set_dim_value(batch_size);
    sequences_shape.add_dim()->set_dim_value(num_return_sequences_value);
    updateOutputShape(ctx, 1, sequences_scores_shape);

    if (ctx.getNumOutputs() > 2) {
      ONNX_NAMESPACE::TensorShapeProto scores_shape;
      scores_shape.add_dim()->set_dim_value(max_length_value - sequence_length);
      scores_shape.add_dim()->set_dim_value(batch_size);
      scores_shape.add_dim()->set_dim_value(num_beams_value);
      scores_shape.add_dim();  // vocab_size is unknown
      updateOutputShape(ctx, 2, scores_shape);
    }
  }
}

void RegisterTextGenerationSchemas() {
  ONNX_CONTRIB_OPERATOR_SCHEMA(BeamSearch)
        .SetDomain(kMSDomain)
        .SinceVersion(1)
        .SetDoc("Beam Search for text generation. Supports GPT-2 decoder.")
        .Attr("eos_token_id", "The id of the end-of-sequence token", AttributeProto::INT)
        .Attr("pad_token_id", "The id of the padding token", AttributeProto::INT)
        .Attr("no_repeat_ngram_size", "no repeat ngrams size", AttributeProto::INT, static_cast<int64_t>(0))
        .Attr("early_stopping", "early stop or not", AttributeProto::INT, static_cast<int64_t>(0))
        .Attr(
            "body",
            "The GPT-2 subgraph with input_ids, position_ids, attention_mask, past_0, past_1, ... as inputs, and logits, present_0, present_1, ... as output",
            AttributeProto::GRAPH)
        .Input(0, "input_ids", "The sequence used as a prompt for the generation. Shape is (batch_size, sequence_length)", "I")
        .Input(1, "max_length", "The maximum length of the sequence to be generated. Shape is (1)", "I")
        .Input(2, "min_length", "The minimum length below which the score of eos_token_id is set to -Inf. Shape is (1)", "I", OpSchema::Optional)
        .Input(3, "num_beams", "Number of beams for beam search. 1 means no beam search. Shape is (1)", "I")
        .Input(4, "num_return_sequences", "The number of returned sequences in the batch. Shape is (1)", "I")
        .Input(5, "temperature", "The value used to module the next token probabilities. Accepts value > 0.0. Shape is (1)", "T")
        .Input(6, "length_penalty",
              "Exponential penalty to the length. Default value 1.0 means no penalty."
              "Value > 1.0 encourages longer sequences, while values < 1.0 produces shorter sequences."
              "Shape is (1,)",
              "T", OpSchema::Optional)
        .Input(7, "repetition_penalty", "The parameter for repetition penalty. Default value 1.0 means no penalty. Accepts value > 0.0. Shape is (1)", "T", OpSchema::Optional)
        .Input(8, "vocab_mask", "Mask of vocabulary. Words that masked with 0 are not allowed to be generated, and 1 is allowed. Shape is (vacab_size)", "M", OpSchema::Optional)
        .Input(9, "prefix_vocab_mask", "Mask of vocabulary for first step. Words that masked with 0 are not allowed to be generated, and 1 is allowed. Shape is (batch_size, vocab_size)", "M", OpSchema::Optional)
        .Output(0, "sequences", "Word IDs of generated sequences. Shape is (batch_size, num_return_sequences, max_sequence_length)", "I")
        .Output(1, "sequences_scores", "Final beam score of the generated sequences. Shape is (batch_size, num_return_sequences)", "T", OpSchema::Optional)
        .Output(2, "scores",
                "Processed beam scores for each vocabulary token at each generation step."
                "Beam scores consisting of log softmax scores for each vocabulary token and sum of log softmax of previously generated tokens in this beam."
                "Shape is (max_length - sequence_length, batch_size, num_beams, vocab_size)",
                "T", OpSchema::Optional)
        .TypeConstraint("T", {"tensor(float)", "tensor(float16)"}, "Constrain input and output types to float tensors.")
        .TypeConstraint("I", {"tensor(int32)"}, "Constrain to integer types")
        .TypeConstraint("M", {"tensor(int32)"}, "Constrain mask to integer types")
        .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
          BeamSearchShapeInference(ctx);
        });
}

void RegisterBertSchemas() {
  static const char* Attention_ver1_doc = R"DOC(
Multi-Head Self Attention that can be either unidirectional (like GPT-2) or bidirectional (like BERT).
The mask_index input is optional. Besides raw attention mask with shape (batch_size, past_sequence_length + sequence_length)
or (batch_size, sequence_length, past_sequence_length + sequence_length) with value 0 for masked and 1 otherwise,
we also support other two formats: When input has right-side padding, mask_index is one dimension with shape (batch_size),
where value of each element is the end position, or valid length of actual sequence excluding padding. When input has
left-side padding, mask_index has shape (2 * batch_size), where the values are the exclusive end positions followed by
the inclusive start positions. When unidirectional is 1, and each token only attend to previous tokens. For GPT-2, both past
and present state are optional. Present state could appear in output even when past state is not in input.
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(Attention)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(Attention_ver1_doc)
      .Attr("num_heads", "Number of attention heads", AttributeProto::INT)
      .Attr("unidirectional",
            "Whether every token can only attend to previous tokens. Default value is 0.",
            AttributeProto::INT,
            static_cast<int64_t>(0))
      .Attr("qkv_hidden_sizes",
            "Hidden layer sizes of Q, K, V paths in Attention",
            AttributeProto::INTS,
            OPTIONAL_VALUE)
      .Input(0, "input", "3D input tensor with shape (batch_size, sequence_length, input_hidden_size)", "T")
      .Input(1, "weight", "2D input tensor with shape (input_hidden_size, 3 * hidden_size), where hidden_size = num_heads * head_size", "T")
      .Input(2, "bias", "1D input tensor with shape (3 * hidden_size)", "T")
      .Input(3, "mask_index",
             "Attention mask with shape (batch_size, 1, max_sequence_length, max_sequence_length), (batch_size, past_sequence_length + sequence_length)"
             "or (batch_size, sequence_length, past_sequence_length + sequence_length), or index with shape (batch_size) or (2 * batch_size).",
             "M", OpSchema::Optional)
      .Input(4, "past", "past state for key and value with shape (2, batch_size, num_heads, past_sequence_length, head_size).", "T", OpSchema::Optional)
      .Input(5, "extra_add", "additional add to QxK' with shape (batch_size, num_heads, sequence_length, sequence_length).", "T", OpSchema::Optional)
      .Output(0, "output", "3D output tensor with shape (batch_size, sequence_length, hidden_size)", "T")
      .Output(1, "present", "present state for key and value with shape (2, batch_size, num_heads, past_sequence_length + sequence_length, head_size)", "T", OpSchema::Optional)
      .TypeConstraint("T", {"tensor(float)", "tensor(float16)"}, "Constrain input and output types to float tensors.")
      .TypeConstraint("M", {"tensor(int32)"}, "Constrain mask index to integer types")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        constexpr int past_input_index = 4;
        AttentionTypeAndShapeInference(ctx, past_input_index);
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(QAttention)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc("Quantization of Multi-Head Self Attention.")
      .Attr("num_heads", "Number of attention heads", AttributeProto::INT)
      .Attr("unidirectional",
            "Whether every token can only attend to previous tokens. Default value is 0.",
            AttributeProto::INT,
            static_cast<int64_t>(0))
      .Input(
          0,
          "input",
          "3D input tensor with shape (batch_size, sequence_length, input_hidden_size)",
          "T1")
      .Input(
          1,
          "weight",
          "2D input tensor with shape (input_hidden_size, 3 * hidden_size), hidden_size = num_heads * head_size",
          "T2")
      .Input(
          2,
          "bias",
          "1D input tensor with shape (3 * hidden_size)",
          "T3")
      .Input(
          3,
          "input_scale",
          "scale of quantized input tensor. It's a scalar, which means a per-tensor/layer quantization.",
          "T3")
      .Input(
          4,
          "weight_scale",
          "scale of weight scale. It's a scalar or a 1D tensor, which means a per-tensor/per-column quantization."
          "Its size should be 3 * hidden_size if it is per-column quantization",
          "T3")
      .Input(
          5,
          "mask_index",
          "Attention mask index with shape (batch_size)",
          "T4",
          OpSchema::Optional)
      .Input(
          6,
          "input_zero_point",
          "zero point of quantized input tensor.It's a scalar, which means a per-tensor/layer quantization.",
          "T1",
          OpSchema::Optional)
      .Input(
          7,
          "weight_zero_point",
          "zero point of quantized weight tensor. It's a scalar or a 1D tensor, which means a per-tensor/per-column quantization."
          "Its size should be 3 * hidden_size if it is per-column quantization",
          "T2",
          OpSchema::Optional)
      .Input(
          8,
          "past",
          "past state for key and value with shape (2, batch_size, num_heads, past_sequence_length, head_size).",
          "T3",
          OpSchema::Optional)
      .Output(
          0,
          "output",
          "3D output tensor with shape (batch_size, sequence_length, hidden_size)",
          "T3")
      .Output(
          1,
          "present",
          "present state for key and value with shape (2, batch_size, num_heads, past_sequence_length + sequence_length, head_size)",
          "T3",
          OpSchema::Optional)
      .TypeConstraint("T1", {"tensor(int8)", "tensor(uint8)"}, "Constrain input and output types to int8 tensors.")
      .TypeConstraint("T2", {"tensor(int8)", "tensor(uint8)"}, "Constrain input and output types to int8 tensors.")
      .TypeConstraint("T3", {"tensor(float)", "tensor(float16)"}, "Constrain input and output types to float tensors.")
      .TypeConstraint("T4", {"tensor(int32)"}, "Constrain mask index to integer types")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        constexpr int past_input_index = 8;

        AttentionTypeAndShapeInference(ctx, past_input_index);
      });

  static const char* Longformer_Attention_doc = R"DOC(
Longformer Self Attention with a local context and a global context. Tokens attend locally: Each token
attends to its W previous tokens and W succeding tokens with W being the window length. A selected few tokens
attend globally to all other tokens.

The attention mask is of shape (batch_size, sequence_length), where sequence_length is a multiple of 2W after padding.
Mask value < 0 (like -10000.0) means the token is masked, 0 otherwise.

Global attention flags have value 1 for the tokens attend globally and 0 otherwise.
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(LongformerAttention)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(Longformer_Attention_doc)
      .Attr("num_heads", "Number of attention heads", AttributeProto::INT)
      .Attr("window", "One sided attention windows length W, or half of total window length", AttributeProto::INT)
      .Input(0, "input", "3D input tensor with shape (batch_size, sequence_length, hidden_size), hidden_size = num_heads * head_size", "T")
      .Input(1, "weight", "2D input tensor with shape (hidden_size, 3 * hidden_size)", "T")
      .Input(2, "bias", "1D input tensor with shape (3 * hidden_size)", "T")
      .Input(3, "mask", "Attention mask with shape (batch_size, sequence_length)", "T")
      .Input(4, "global_weight", "2D input tensor with shape (hidden_size, 3 * hidden_size)", "T")
      .Input(5, "global_bias", "1D input tensor with shape (3 * hidden_size)", "T")
      .Input(6, "global", "Global attention flags with shape (batch_size, sequence_length)", "G")
      .Output(0, "output", "3D output tensor with shape (batch_size, sequence_length, hidden_size)", "T")
      .TypeConstraint("T", {"tensor(float)", "tensor(float16)"}, "Constrain input and output types to float tensors.")
      .TypeConstraint("G", {"tensor(int32)"}, "Constrain to integer types")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  static const char* Decoder_Attention_doc = R"DOC(
This DecoderAttention supports self attention and cross attention, key and value cache, and key_padding_mask. The attention mask is not support at the moment.
Some boolean parameters are passed by runtime input for generic purpose
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(DecoderAttention)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(Decoder_Attention_doc)
      .Attr("num_heads", "Number of attention heads", AttributeProto::INT)
      .Input(0, "query", "3D input tensor with shape (sequence_length, batch_size, hidden_size), hidden_size = num_heads * head_size", "T")
      .Input(1, "key", "3D input tensor with shape (total_sequence_length, batch_size, hidden_size)", "T")
      .Input(2, "q_weight", "2D input tensor with shape (hidden_size, hidden_size)", "T")
      .Input(3, "kv_weight", "2D input tensor with shape (hidden_size, 2 * hidden_size)", "T")
      .Input(4, "bias", "1D input tensor with shape (3 * hidden_size)", "T")
      .Input(5, "key_padding_mask", "2D input tensor with shape (batch_size, total_sequence_length)", "B", OpSchema::Optional)
      .Input(6, "key_cache", "input tensor with shape (batch_size, num_heads, sequence_length or total_sequence_length, head_size)", "T", OpSchema::Optional)   // self & cross
      .Input(7, "value_cache", "input tensor with shape (batch_size, num_heads, sequence_length or total_sequence_length, head_size)", "T", OpSchema::Optional)   // self & cross
      .Input(8, "static_kv", "If static_kv = true, cross-attention; else self-attention", "B")
      .Input(9, "use_past", "If use_past = true, use cache; else no cache", "B")
      .Input(10, "has_layer_state", "If has_layer_state = true, layer_state = {} or [a,b]; else layer_state = None", "B")
      .Input(11, "has_key_padding_mask", "has_key_padding_mask or not", "B")
      .Output(0, "output", "3D output tensor with shape (sequence_length, batch_size, hidden_size)", "T")
      .Output(1, "new_key_cache", "output tensor with shape (batch_size, num_heads, new sequence_length, head_size)", "T", OpSchema::Optional) // self & cross
      .Output(2, "new_value_cache", "output tensor with shape (batch_size, num_heads, new sequence_length, head_size)", "T", OpSchema::Optional) // self & cross
      .TypeConstraint("T", {"tensor(float)", "tensor(float16)"}, "Constrain input and output types to float and float16 tensors.")
      .TypeConstraint("B", {"tensor(bool)"}, "Constrain key_padding_mask to bool tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        DecoderAttentionTypeAndShapeInference(ctx);
      });

  static const char* EmbedLayerNormalization_ver1_doc = R"DOC(
EmbedLayerNormalization is the fusion of embedding layer in BERT model, with optional mask processing.
The embedding layer takes input_ids (word IDs) and segment_ids (sentence IDs) to look up word_embedding, position_embedding,
and segment_emedding; the embeddings are added then applied layer normalization using gamma and beta tensors.
The last input mask is optional. If mask is provided, mask index (that is position of first 0 in mask, or number of words)
will be calculated.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(EmbedLayerNormalization)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(EmbedLayerNormalization_ver1_doc)
      .Attr("epsilon", "The epsilon value to use to avoid division by zero.", AttributeProto::FLOAT, kDefaultEmbedLayerNormEpsilon)
      .Input(0, "input_ids", "2D words IDs with shape (batch_size, sequence_length)", "T1")
      .Input(1, "segment_ids", "2D segment IDs with shape (batch_size, sequence_length)", "T1", OpSchema::Optional)
      .Input(2, "word_embedding", "2D with shape (,hidden_size)", "T")
      .Input(3, "position_embedding", "2D with shape (, hidden_size)", "T")
      .Input(4, "segment_embedding", "2D with shape (, hidden_size)", "T", OpSchema::Optional)
      .Input(5, "gamma", "1D gamma tensor for layer normalization with shape (hidden_size)", "T")
      .Input(6, "beta", "1D beta tensor for layer normalization  with shape (hidden_size)", "T")
      .Input(7, "mask", "2D attention mask with shape (batch_size, sequence_length)", "T1", OpSchema::Optional)
      .Input(8, "position_ids", "2D position ids with shape (batch_size, sequence_length)", "T1", OpSchema::Optional)
      .Output(0, "output", "3D output tensor with shape (batch_size, sequence_length, hidden_size)", "T")
      .Output(1, "mask_index", "1D mask_index tensor with shape (batch_size)", "T1")
      .Output(2, "embedding_sum", "sum of word_embedding and position_embedding without layer normalization", "T", OpSchema::Optional)
      .TypeConstraint("T1", {"tensor(int32)"}, "Constrain input and output integer tensors types")
      .TypeConstraint("T", {"tensor(float)", "tensor(float16)"}, "Constrain input and output float tensors types.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::embedLayerNormalizationShapeInference);

  static const char* QEmbedLayerNormalization_ver1_doc = R"DOC(
QEmbedLayerNormalization is the quantized fusion of embedding layer in BERT model, with optional mask processing.
The embedding layer takes input_ids (word IDs) and segment_ids (sentence IDs) to look up word_embedding, position_embedding,
and segment_emedding; the embeddings are added then applied layer normalization using gamma and beta tensors. The input_ids
and segment_ids remain int32. All embeddings, gamma, and beta tensors are converted to int8/uint8. The last input mask is optional.
If mask is provided, mask index (that is position of first 0 in mask, or number of words will be calculated.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(QEmbedLayerNormalization)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetSupportLevel(OpSchema::SupportType::EXPERIMENTAL)
      .SetDoc(QEmbedLayerNormalization_ver1_doc)
      .Attr("epsilon", "The epsilon value to use to avoid division by zero.", AttributeProto::FLOAT, kDefaultEmbedLayerNormEpsilon)
      .Input(0, "input_ids", "2D words IDs with shape (batch_size, sequence_length)", "T1")
      .Input(1, "segment_ids", "2D segment IDs with shape (batch_size, sequence_length)", "T1", OpSchema::Optional)
      .Input(2, "word_embedding_quant", "2D with shape (,hidden_size)", "T2")
      .Input(3, "position_embedding_quant", "2D with shape (, hidden_size)", "T2")
      .Input(4, "segment_embedding", "2D with shape (, hidden_size)", "T2", OpSchema::Optional)
      .Input(5, "gamma_quant", "1D gamma tensor for layer normalization with shape (hidden_size)", "T2")
      .Input(6, "beta_quant", "1D beta tensor for layer normalization  with shape (hidden_size)", "T2")
      .Input(7, "mask", "Mask", "T1", OpSchema::Optional)
      .Input(8, "word_embedding_scale", "Scale for word embeddings", "T")
      .Input(9, "position_embedding_scale", "Scale for position embeddings", "T")
      .Input(10, "segment_embedding_scale", "Scale for segment embeddings", "T", OpSchema::Optional)
      .Input(11, "gamma_scale", "Scale for 1D gamma tensor", "T")
      .Input(12, "beta_scale", "Scale for 1D beta tensor", "T")
      .Input(13, "word_embedding_zero_point", "Zero point for word embeddings", "T2")
      .Input(14, "position_embedding_zero_point", "Zero point for position embeddings", "T2")
      .Input(15, "segment_embedding_zero_point", "Zero Point for segment embeddings", "T2", OpSchema::Optional)
      .Input(16, "gamma_zero_point", "Zero Point for 1D gamma tensor", "T2")
      .Input(17, "beta_zero_point", "Zero Point for 1D beta tensor", "T2")
      .Output(0, "layernorm_out", "LayerNorm Output", "T")
      .Output(1, "mask_index_out", "Mask Index Output", "T1")
      .TypeConstraint("T1", {"tensor(int32)"}, "Constrain mask index to integer types")
      .TypeConstraint("T2", {"tensor(int8)", "tensor(uint8)"}, "Constrain input and output types to int8 tensors.")
      .TypeConstraint("T", {"tensor(float)"}, "Constrain input and output types to float32 tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::embedLayerNormalizationShapeInference);

  static const char* FastGelu_ver1_doc = R"DOC(
GELU (Gaussian Error Linear Unit) approximation: Y=0.5*X*(1+tanh(0.797885*X+0.035677*X*X*X)) with an optional input of bias that will be added to X before GELU.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(FastGelu)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(FastGelu_ver1_doc)
      .Input(0, "X", "input tensor", "T")
      .Input(1, "bias", "bias tensor", "T", OpSchema::Optional)
      .Output(0, "Y", "output tensor", "T")
      .TypeConstraint("T", {"tensor(float)", "tensor(float16)", "tensor(bfloat16)"}, "Constrain input and output types to float or half tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput)
      .SetContextDependentFunctionBodyBuilder([](const FunctionBodyBuildContext& ctx, const OpSchema& schema, FunctionProto& functionProto) {
        // fastgelu(x) =
        auto* tp = ctx.getInputType(0);
        if ((tp == nullptr) || (!tp->has_tensor_type()))
          return false;
        auto elem_type = tp->tensor_type().elem_type();

        // Optional input 1 indicates a bias to be added to input 0.
        auto hasBias = ctx.hasInput(1);

        FunctionBuilder builder(functionProto);
        builder
            .AddOpset("", 13)
            .Const("a", 0.5, elem_type)
            .Const("b", 0.797885, elem_type)
            .Const("c", 0.035677, elem_type)
            .Const("one", 1.0, elem_type)
            .Add(hasBias ? "X_bias = Add (X, bias)" : "X_bias = Identity (X)")
            .Add(R"(
                T1 = Mul (X_bias, X_bias)
                T2 = Mul (c, T1)
                T3 = Add (b, T2)
                T4 = Mul (X_bias, T3)
                T5 = Tanh (T4)
                T6 = Add (one, T5)
                T7 = Mul (X_bias, T6)
                Y = Mul (a, T7)
            )");

        schema.BuildFunction(functionProto);
        return true;
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(SkipLayerNormalization)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc("Skip and Layer Normalization Fusion")
      .Attr("epsilon", "The epsilon value to use to avoid division by zero.", AttributeProto::FLOAT, kDefaultSkipLayerNormEpsilon)
      .Input(0, "input", "3D input tensor with shape (batch_size, sequence_length, hidden_size)", "T")
      .Input(1, "skip", "3D skip tensor with shape (batch_size, sequence_length, hidden_size)", "T")
      .Input(2, "gamma", "1D input tensor with shape (hidden_size)", "T")
      .Input(3, "beta", "1D skip tensor with shape (hidden_size", "T", OpSchema::Optional)
      .Input(4, "bias", "1D bias tensor with shape (hidden_size", "T", OpSchema::Optional)
      .Output(0, "output", "3D output tensor with shape (batch_size, sequence_length, hidden_size)", "T")
      .Output(1, "mean", "Saved mean used during training to speed up gradient computation", "U", OpSchema::Optional)
      .Output(2, "inv_std_var", "Saved inverse standard variance used during training to speed up gradient computation.", "U", OpSchema::Optional)
      .TypeConstraint("T", {"tensor(float)", "tensor(float16)"}, "Constrain input and output types to float or half tensors.")
      .TypeConstraint("U", {"tensor(float)"}, "Constrain mean and inv_std_var to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  static const char* NGramRepeatBlock_ver1_doc = R"DOC(
Enforce no repetition of n-grams. Scores are set to `-inf` for tokens that form a repeated n-gram if added to the back of the input_ids.
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(NGramRepeatBlock)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(NGramRepeatBlock_ver1_doc)
      .Attr("ngram_size", "The NGram size.", AttributeProto::INT)
      .Input(0, "input_ids", "2D input tensor with shape (batch_size, sequence_length)", "Tid")
      .Input(1, "scores", "2D input tensor with shape (batch_size, vocab_size)", "T")
      .Output(0, "scores_out", "2D output tensor with shape (batch_size, vocab_size)", "T")
      .TypeConstraint("Tid", {"tensor(int64)"}, "Constrain indices to integer types")
      .TypeConstraint("T", {"tensor(float)"}, "Constrain scores input and output types to float tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        propagateElemTypeFromInputToOutput(ctx, 1, 0);
        if (!hasInputShape(ctx, 1)) {
          return;
        }
        propagateShapeFromInputToOutput(ctx, 1, 0);
      });

  static const char* BifurcationDetector_ver1_doc = R"DOC(
Component for aggressive decoding. Find the bifurcation index of predicted tokens, between source tokens,
starting from previous suffix match index, and predicted tokens.
Concat predicted tokens, starting from bifurcation index, to the back
of current tokens. This forms the output tokens.
Detect suffix match index in source tokens, between source tokens and output tokens.
Detection is based on finding the appearances of last n-gram in output tokens
in source tokens.
A match is considered found if source tokens contain a single matching n-gram.
Return the index of the start of the n-gram in source tokens.
No matching if found if src tokens contain multiple or zero matching n-grams. Return -1.
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(BifurcationDetector)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(BifurcationDetector_ver1_doc)
      .Attr("min_ngram_size", "The minimum NGram size for suffix matching.", AttributeProto::INT, static_cast<int64_t>(1))
      .Attr("max_ngram_size", "The maximum NGram size for suffix matching.", AttributeProto::INT, static_cast<int64_t>(3))
      .Input(0, "src_tokens", "Encoder input ids.", "T")
      .Input(1, "cur_tokens", "Decoder input ids.", "T")
      .Input(2, "prev_suffix_match_idx", "Previous suffix match index", "T")
      .Input(3, "pred_tokens", "Predicted token ids from aggressive decoding", "T", OpSchema::Optional)
      .Output(0, "tokens", "Decoder input ids after merging predicted tokens", "T")
      .Output(1, "suffix_match_idx", "new suffix match index", "T")
      .TypeConstraint("T", {"tensor(int64)"}, "Constrain to integer types.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        propagateElemTypeFromInputToOutput(ctx, 1, 0);
        propagateElemTypeFromInputToOutput(ctx, 2, 1);
        if (hasInputShape(ctx, 2)) {
          propagateShapeFromInputToOutput(ctx, 2, 1);
        }
        // output tokens lengths is dynamic as it depends on the bifurcation index of predicted tokens and source tokens,
        // and current tokens length.
        // tokens_length = cur_tokens_length + bifurcation_index + 1.
      });
}

void RegisterContribSchemas() {
  // Register removed experimental ops for backward compatibility.
  // Experimental operators do not have version history. However, RS5 takes bunch of experimental operators
  // as production ops. In order to maintain backward compatibility when the experimental ops are removed from ONNX
  // they need to be added in onnxruntime as contrib ops.
  // ONNX exp ops(Affine, Crop, ParametricSoftplus, ImageScaler, ThresholdedRelu, DynamicSlice, ScaledTanh, MVN) old version history maintenance
  static const char* Affine_ver1_doc = R"DOC(
Affine takes one input data (Tensor<T>) and produces one output data
(Tensor<T>) where the affine function, y = alpha * x + beta,
is applied to the tensor elementwise.
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(Affine)
      .SinceVersion(1)
      .SetDoc(Affine_ver1_doc)
      .Attr("alpha", "Value of alpha", AttributeProto::FLOAT, 1.0f)
      .Attr("beta", "Value of beta", AttributeProto::FLOAT, 0.0f)
      .Input(0, "X", "1D input tensor", "T")
      .Output(0, "Y", "1D output tensor", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  static const char* ParametricSoftplus_ver1_doc = R"DOC(
ParametricSoftplus takes one input data (Tensor<T>) and produces one output data
(Tensor<T>) where the softplus function, y = alpha * ln(exp(beta * x) + 1), is applied to
the tensor elementwise.
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(ParametricSoftplus)
      .SinceVersion(1)
      .SetDoc(ParametricSoftplus_ver1_doc)
      .Attr("alpha", "Value of alpha", AttributeProto::FLOAT, OPTIONAL_VALUE)
      .Attr("beta", "Value of beta", AttributeProto::FLOAT, OPTIONAL_VALUE)
      .Input(0, "X", "1D input tensor", "T")
      .Output(0, "Y", "1D input tensor", "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  static const char* ImageScaler_ver1_doc =
      R"DOC(Scale and bias the input image. Bias values are stored in
the same ordering as the image pixel format.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(ImageScaler)
      .SinceVersion(1)
      .SetDoc(ImageScaler_ver1_doc)
      .Attr("bias", "Bias applied to each channel, same size as C.", AttributeProto::FLOATS, OPTIONAL_VALUE)
      .Attr("scale", "The scale to apply.", AttributeProto::FLOAT, 1.0f)
      .Input(0, "input", "Input tensor of shape [N,C,H,W]", "T")
      .Output(0, "output", "Result, has same shape and type as input", "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  static const char* Crop_ver1_doc =
      R"DOC(Crop and image to the specified spatial dimensions. If scale is given,
then optionally start the crop offset by the left/top border amounts.
If scale is not provided, crop the borders as provided.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(Crop)
      .SinceVersion(1)
      .SetDoc(Crop_ver1_doc)
      .Attr("border", "A 1-D values of (leftBorder, topBorder, rightBorder, bottomBorder).", AttributeProto::INTS, OPTIONAL_VALUE)
      .Attr("scale", "A 1-D values of (height, width).", AttributeProto::INTS, OPTIONAL_VALUE)
      .Input(0, "input", "Input tensor of shape [N,C,H,W]", "T")
      .Output(0, "output", "Result, has same type as input, with H and W dimensions reduced.", "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors.");

  static const char* ThresholdedRelu_ver1_doc = R"DOC(
ThresholdedRelu takes one input data (Tensor<T>) and produces one output data
(Tensor<T>) where the rectified linear function, y = x for x > alpha, y = 0 otherwise,
is applied to the tensor elementwise. )DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(ThresholdedRelu)
      .SinceVersion(1)
      .SetDoc(ThresholdedRelu_ver1_doc)
      .Attr("alpha", "Threshold value", AttributeProto::FLOAT, 1.0f)
      .Input(0, "X", "Input tensor", "T")
      .Output(0, "Y", "Output tensor", "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  static const char* DynamicSlice_ver1_doc = R"DOC(
Produces a slice of the input tensor along multiple axes. Similar to numpy:
https://docs.scipy.org/doc/numpy/reference/arrays.indexing.html
Slices uses `axes`, `starts` and `ends` inputs to specify the start and end
dimension for each axis in the list of axes, it uses this information to
slice the input `data` tensor. If a negative value is passed for any of the
start or end indices, it represent number of elements before the end of that
dimension. If the value passed to start or end is larger than the `n` (the
number of elements in this dimension), it represents `n`. For slicing to the
end of a dimension with unknown size, it is recommended to pass in `INT_MAX`.
If `axes` are omitted, they are set to `[0, ..., ndim-1]`.
Example 1:
  data = [
      [1, 2, 3, 4],
      [5, 6, 7, 8],
  ]
  axes = [0, 1]
  starts = [1, 0]
  ends = [2, 3]
  result = [
      [5, 6, 7],
  ]
Example 2:
  data = [
      [1, 2, 3, 4],
      [5, 6, 7, 8],
  ]
  starts = [0, 1]
  ends = [-1, 1000]
  result = [
      [2, 3, 4],
  ]
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(DynamicSlice)
      .SinceVersion(1)
      .SetDoc(DynamicSlice_ver1_doc)
      .Input(0, "data", "Tensor of data to extract slices from.", "T")
      .Input(1, "starts", "1-D tensor of starting indices of corresponding axis in `axes`", "Tind")
      .Input(2, "ends", "1-D tensor of ending indices (exclusive) of corresponding axis in axes", "Tind")
      .Input(3, "axes", "1-D tensor of axes that `starts` and `ends` apply to.", "Tind", OpSchema::Optional)
      .Output(0, "output", "Sliced data tensor.", "T")
      .TypeConstraint("T", OpSchema::all_tensor_types(), "Constrain input and output types to all tensor types.")
      .TypeConstraint("Tind", {"tensor(int32)", "tensor(int64)"}, "Constrain indices to integer types");

  ONNX_CONTRIB_OPERATOR_SCHEMA(GivenTensorFill)
      .SinceVersion(1)
      .Input(0, "shape", "The shape of filled tensor", "T", OpSchema::Optional)
      .Output(0, "X", "The filled tensor", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .Attr("values", "", AttributeProto::FLOATS, OPTIONAL_VALUE)
      .Attr("shape", "", AttributeProto::INTS, OPTIONAL_VALUE)
      .Attr("input_as_shape", "", AttributeProto::INT, OPTIONAL_VALUE)
      .Attr("extra_shape", "", AttributeProto::INTS, OPTIONAL_VALUE)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 0);
        if (ctx.getAttribute("shape") != nullptr) {
          propagateShapeFromAttributeToOutput(ctx, "shape", 0);
          return;
        }
        // The type constraints above do not allow for input_as_shape
        // and may need to be fixed.
        if (getAttribute(ctx, "input_as_shape", 0) != 0)  // dynamic shape
          return;
        std::vector<int64_t> extra_shape;
        getRepeatedAttribute(ctx, "extra_shape", extra_shape);
        if (hasInputShape(ctx, 0)) {
          ONNX_NAMESPACE::TensorShapeProto shape = ctx.getInputType(0)->tensor_type().shape();
          for (auto extra_dim_val : extra_shape) {
            if (extra_dim_val < 0)
              fail_shape_inference(
                  "Negative values are not allowed in a shape specification");
            shape.add_dim()->set_dim_value(extra_dim_val);
          }
          updateOutputShape(ctx, 0, shape);
        }
      });

  static const char* Scale_ver1_doc = R"DOC(
Scale takes one input data (Tensor<float>) and produces one output data
(Tensor<float>) whose value is the input data tensor scaled element-wise.
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(Scale)
      .SinceVersion(1)
      .Input(0, "input", "Input data to be scaled", "T")
      .Output(0, "output", "Output data after scaling", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .SetDoc(Scale_ver1_doc)
      .Attr("scale", "The scale to apply.", AttributeProto::FLOAT, 1.0f)
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  static const char* GRUUnit_ver1_doc = R"DOC(
GRUUnit computes the activations of a standard GRU,
in a sequence-length aware fashion.
Concretely, given the (fused) inputs X (TxNxD), the previous hidden
state (NxD), and the sequence lengths (N), computes the GRU
activations, avoiding computation if the input is invalid (as in, the
value at X[t][n] >= seqLengths[n].
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(GRUUnit)
      .SinceVersion(1)
      .SetDoc(GRUUnit_ver1_doc)
      .Attr("drop_states",
            "Bool to determine if hidden state is zeroes or passed "
            "along for timesteps past the given sequence_length.",
            AttributeProto::INT, OPTIONAL_VALUE)
      .Input(0, "hidden_prev", "The previous GRU hidden state.", "T")
      .Input(
          1,
          "gates",
          "Unactivated gate outputs from forget, update, "
          "and output gates, pre-activation.",
          "T")
      .Input(
          2,
          "seq_lengths",
          "Array of sequence lengths.  "
          "len(seq_lengths) should equal batch size N.",
          "T")
      .Input(3, "t", "The timestep for this operation.", "T")
      .Output(
          0,
          "hidden",
          "The new GRU hidden state calculated by this op.",
          "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.");

  ONNX_CONTRIB_OPERATOR_SCHEMA(GivenTensorFill)
      .SinceVersion(10)
      .Deprecate()
      .Input(0, "shape", "The shape of filled tensor", "T", OpSchema::Optional)
      .Output(0, "X", "The filled tensor", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .Attr("values", "", AttributeProto::FLOATS, OPTIONAL_VALUE)
      .Attr("shape", "", AttributeProto::INTS, OPTIONAL_VALUE)
      .Attr("input_as_shape", "", AttributeProto::INT, OPTIONAL_VALUE)
      .Attr("extra_shape", "", AttributeProto::INTS, OPTIONAL_VALUE)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 0);
        if (ctx.getAttribute("shape") != nullptr) {
          propagateShapeFromAttributeToOutput(ctx, "shape", 0);
          return;
        }
        // The type constraints above do not allow for input_as_shape
        // and may need to be fixed.
        if (getAttribute(ctx, "input_as_shape", 0) != 0)  // dynamic shape
          return;
        std::vector<int64_t> extra_shape;
        getRepeatedAttribute(ctx, "extra_shape", extra_shape);
        if (hasInputShape(ctx, 0)) {
          ONNX_NAMESPACE::TensorShapeProto shape = ctx.getInputType(0)->tensor_type().shape();
          for (auto extra_dim_val : extra_shape) {
            if (extra_dim_val < 0)
              fail_shape_inference(
                  "Negative values are not allowed in a shape specification");
            shape.add_dim()->set_dim_value(extra_dim_val);
          }
          updateOutputShape(ctx, 0, shape);
        }
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(Scale)
      .SinceVersion(10)
      .Deprecate()
      .Input(0, "input", "Input data to be scaled", "T")
      .Output(0, "output", "Output data after scaling", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .SetDoc(Scale_ver1_doc)
      .Attr("scale", "The scale to apply.", AttributeProto::FLOAT, 1.0f)
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  ONNX_CONTRIB_OPERATOR_SCHEMA(GRUUnit)
      .SinceVersion(10)
      .Deprecate()
      .SetDoc(GRUUnit_ver1_doc)
      .Attr("drop_states",
            "Bool to determine if hidden state is zeroes or passed "
            "along for timesteps past the given sequence_length.",
            AttributeProto::INT, OPTIONAL_VALUE)
      .Input(0, "hidden_prev", "The previous GRU hidden state.", "T")
      .Input(
          1,
          "gates",
          "Unactivated gate outputs from forget, update, "
          "and output gates, pre-activation.",
          "T")
      .Input(
          2,
          "seq_lengths",
          "Array of sequence lengths.  "
          "len(seq_lengths) should equal batch size N.",
          "T")
      .Input(3, "t", "The timestep for this operation.", "T")
      .Output(
          0,
          "hidden",
          "The new GRU hidden state calculated by this op.",
          "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.");

  ONNX_OPERATOR_SCHEMA(MeanVarianceNormalization)
      .SinceVersion(1)
      .SetDoc(R"DOC(Perform mean variance normalization.)DOC")
      .Attr("across_channels", "If 1, mean and variance are computed across channels. Default is 0.", AttributeProto::INT, static_cast<int64_t>(0))
      .Attr("normalize_variance", "If 0, normalize the mean only.  Default is 1.", AttributeProto::INT, static_cast<int64_t>(1))
      .Input(0, "input", "Input tensor of shape [N,C,H,W]", "T")
      .Output(0, "output", "Result, has same shape and type as input", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  ONNX_OPERATOR_SCHEMA(ScaledTanh)
      .SinceVersion(1)
      .Attr("alpha", "Scaling value", AttributeProto::FLOAT, OPTIONAL_VALUE)
      .Attr("beta", "Scaling value", AttributeProto::FLOAT, OPTIONAL_VALUE)
      .Input(0, "input", "Input tensor", "T")
      .Output(
          0,
          "output",
          "The scaled hyperbolic tangent values of the input tensor "
          "computed element-wise",
          "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  ONNX_CONTRIB_OPERATOR_SCHEMA(Affine)
      .SinceVersion(10)
      .Deprecate()
      .SetDoc(Affine_ver1_doc)
      .Attr("alpha", "Value of alpha", AttributeProto::FLOAT, 1.0f)
      .Attr("beta", "Value of beta", AttributeProto::FLOAT, 0.0f)
      .Input(0, "X", "1D input tensor", "T")
      .Output(0, "Y", "1D output tensor", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  ONNX_CONTRIB_OPERATOR_SCHEMA(ParametricSoftplus)
      .SinceVersion(10)
      .Deprecate()
      .SetDoc(ParametricSoftplus_ver1_doc)
      .Attr("alpha", "Value of alpha", AttributeProto::FLOAT, OPTIONAL_VALUE)
      .Attr("beta", "Value of beta", AttributeProto::FLOAT, OPTIONAL_VALUE)
      .Input(0, "X", "1D input tensor", "T")
      .Output(0, "Y", "1D input tensor", "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  ONNX_CONTRIB_OPERATOR_SCHEMA(ImageScaler)
      .SinceVersion(10)
      .Deprecate()
      .SetDoc(ImageScaler_ver1_doc)
      .Attr("bias", "Bias applied to each channel, same size as C.", AttributeProto::FLOATS, OPTIONAL_VALUE)
      .Attr("scale", "The scale to apply.", AttributeProto::FLOAT, 1.0f)
      .Input(0, "input", "Input tensor of shape [N,C,H,W]", "T")
      .Output(0, "output", "Result, has same shape and type as input", "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  ONNX_CONTRIB_OPERATOR_SCHEMA(Crop)
      .SinceVersion(10)
      .Deprecate()
      .SetDoc(Crop_ver1_doc)
      .Attr("border", "A 1-D values of (leftBorder, topBorder, rightBorder, bottomBorder).", AttributeProto::INTS)
      .Attr("scale", "A 1-D values of (height, width).", AttributeProto::INTS, OPTIONAL_VALUE)
      .Input(0, "input", "Input tensor of shape [N,C,H,W]", "T")
      .Output(0, "output", "Result, has same type as input, with H and W dimensions reduced.", "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        // Type inference
        ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 0);

        // Shape inference
        auto* output_shape =
            ctx.getOutputType(0)->mutable_tensor_type()->mutable_shape();

        if (ONNX_NAMESPACE::hasNInputShapes(ctx, 1)) {
          const auto& input_shape =
              ctx.getInputType(0)->tensor_type().shape();
          const auto input_rank =
              input_shape.dim_size();
          if (input_rank != 4)
            fail_shape_inference("Input's shape must be 4-D");

          // parse necessary attributes for futher processing
          std::vector<int64_t> border;
          bool border_present =
              getRepeatedAttribute(ctx, "border", border);
          if (!border_present || border.size() != 4)
            fail_shape_inference(
                "'Border' attribute must be present and must contain exactly 4 values - "
                "(left_border, top_border, right_border, bottom_border)");

          std::vector<int64_t> scale;
          bool scale_present =
              getRepeatedAttribute(ctx, "scale", scale);
          if (scale_present && scale.size() != 2)
            fail_shape_inference("'Scale' must contain exactly 2 values - (height, width)");

          // actual shape inference processing
          // [N, C] can be copied over from the input as is
          *output_shape->mutable_dim(static_cast<int>(0)) = input_shape.dim(static_cast<int>(0));
          *output_shape->mutable_dim(static_cast<int>(1)) = input_shape.dim(static_cast<int>(1));

          // process 'H' and 'W'
          if (!utils::HasDimValue(input_shape.dim(static_cast<int>(2))) ||
              !utils::HasDimValue(input_shape.dim(static_cast<int>(3)))) {
            // either height and width input has symbolic dims, so can't proceed further
            // add two dims as placeholders for output_H and output_W and return
            output_shape->add_dim();
            output_shape->add_dim();
            return;
          }

          int64_t H = input_shape.dim(static_cast<int>(2)).dim_value();
          int64_t W = input_shape.dim(static_cast<int>(3)).dim_value();

          int64_t left_border = border[0],
                  top_border = border[1],
                  right_border = border[2],
                  bottom_border = border[3];

          if (H < top_border + bottom_border)
            fail_shape_inference("Input's height (", H,
                                 ") needs to be greater than or equal to "
                                 "the top_border (",
                                 top_border, ") + bottom_border (", bottom_border, ")");

          if (W < left_border + right_border)
            fail_shape_inference("Input's width (", W,
                                 ") needs to be greater than or equal to "
                                 "the left_border (",
                                 left_border, ") + right_border (", right_border, ")");

          int64_t bottom_limit = H - bottom_border;
          int64_t right_limit = W - right_border;

          // scale = (height, width)
          if (!scale.empty()) {
            bottom_limit = top_border + scale[0];
            right_limit = left_border + scale[1];

            if (H < bottom_limit)
              fail_shape_inference("Input's height (", H, ") needs to be greater than or equal to the top_border (", top_border, ") + scale[0] (", scale[0], ")");

            if (W < right_limit)
              fail_shape_inference("Input's width (", W, ") needs to be greater than or equal to the left_border (", left_border, ") + scale[1] (", scale[1], ")");
          }

          auto* h_output_dim = output_shape->add_dim();
          h_output_dim->set_dim_value(bottom_limit - top_border);

          auto* w_output_dim = output_shape->add_dim();
          w_output_dim->set_dim_value(right_limit - left_border);
        } else {
          // Rank Inference at the very least
          // (We know that the output is going to be 4-D)
          for (int i = 0; i < 4; ++i) {
            output_shape->add_dim();
          }
        }
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(DynamicSlice)
      .SinceVersion(10)
      .Deprecate()
      .SetDoc(DynamicSlice_ver1_doc)
      .Input(0, "data", "Tensor of data to extract slices from.", "T")
      .Input(1, "starts", "1-D tensor of starting indices of corresponding axis in `axes`", "Tind")
      .Input(2, "ends", "1-D tensor of ending indices (exclusive) of corresponding axis in axes", "Tind")
      .Input(3, "axes", "1-D tensor of axes that `starts` and `ends` apply to.", "Tind", OpSchema::Optional)
      .Output(0, "output", "Sliced data tensor.", "T")
      .TypeConstraint("T", OpSchema::all_tensor_types(), "Constrain input and output types to all tensor types.")
      .TypeConstraint("Tind", {"tensor(int32)", "tensor(int64)"}, "Constrain indices to integer types");

  ONNX_OPERATOR_SCHEMA(ScaledTanh)
      .SinceVersion(10)
      .Deprecate()
      .Attr("alpha", "Scaling value", AttributeProto::FLOAT, OPTIONAL_VALUE)
      .Attr("beta", "Scaling value", AttributeProto::FLOAT, OPTIONAL_VALUE)
      .Input(0, "input", "Input tensor", "T")
      .Output(
          0,
          "output",
          "The scaled hyperbolic tangent values of the input tensor "
          "computed element-wise",
          "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  // End of ONNX exp ops(Affine, Crop, ParametricSoftplus, ImageScaler, ThresholdedRelu, DynamicSlice, ScaledTanh, MVN) old version history maintenance

  ONNX_CONTRIB_OPERATOR_SCHEMA(SampleOp)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "X", "input", "T")
      .Output(0, "Y", "output", "T")
      .TypeConstraint(
          "T",
          ONNX_NAMESPACE::OpSchema::numeric_types_for_math_reduction(),
          "Constrain to any tensor type. If the dtype attribute is not provided this must be a valid output type.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput)
      .SetDoc(R"DOC(
Sample echo operator.)DOC");

  // register schemas for more operators here
  ONNX_CONTRIB_OPERATOR_SCHEMA(MaxpoolWithMask)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC(For internal use.)DOC")
      .Attr(
          "auto_pad",
          "",
          AttributeProto::STRING,
          std::string("NOTSET"))
      .Attr(
          "kernel_shape",
          "",
          AttributeProto::INTS,
          OPTIONAL_VALUE)
      .Attr("pads",
            "",
            AttributeProto::INTS, OPTIONAL_VALUE)
      .Attr(
          "storage_order",
          "",
          AttributeProto::INT,
          static_cast<int64_t>(0))
      .Attr(
          "strides", "", AttributeProto::INTS, OPTIONAL_VALUE)
      .Input(
          0,
          "X",
          "",
          "T")
      .Input(1, "M", "mask", "tensor(int32)")
      .Output(
          0,
          "Y",
          "",
          "T")
      .TypeConstraint("T", {"tensor(float)"}, "Constrain input0 and output types to float tensors")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 0);
        ONNX_NAMESPACE::convPoolShapeInference(ctx, false, true, 0, 1);
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(Rfft)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC()DOC")
      .Input(0, "X", "input tensor", "T")
      .Attr("signal_ndim", "", AttributeProto::INT, static_cast<int64_t>(1))
      .Attr("normalized", "", AttributeProto::INT, static_cast<int64_t>(0))
      .Attr("onesided", "", AttributeProto::INT, static_cast<int64_t>(1))
      .Output(0, "Y", "output tensor", "T")
      .TypeConstraint("T", {"tensor(float)", "tensor(double)", "tensor(float16)"}, "Constrain input and output types to float or half tensors.");

  ONNX_CONTRIB_OPERATOR_SCHEMA(Irfft)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC()DOC")
      .Input(0, "X", "input tensor", "T")
      .Attr("signal_ndim", "", AttributeProto::INT)
      .Attr("normalized", "", AttributeProto::INT, static_cast<int64_t>(0))
      .Attr("onesided", "", AttributeProto::INT, static_cast<int64_t>(1))
      .Output(0, "Y", "output tensor", "T")
      .TypeConstraint("T", {"tensor(float)", "tensor(double)", "tensor(float16)"}, "Constrain input and output types to float or half tensors.");

  ONNX_CONTRIB_OPERATOR_SCHEMA(ComplexMul)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC()DOC")
      .Input(0, "A", "input_0", "T")
      .Input(1, "B", "input_1", "T")
      .Output(0, "C", "output tensor", "T")
      .TypeConstraint("T", {"tensor(float)", "tensor(double)", "tensor(float16)"}, "Constrain input and output types to float or half tensors.");

  ONNX_CONTRIB_OPERATOR_SCHEMA(ComplexMulConj)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC()DOC")
      .Input(0, "A", "input_0", "T")
      .Input(1, "B", "input_1", "T")
      .Output(0, "C", "output tensor", "T")
      .TypeConstraint("T", {"tensor(float)", "tensor(double)", "tensor(float16)"}, "Constrain input and output types to float or half tensors.");

  ONNX_CONTRIB_OPERATOR_SCHEMA(ConvTransposeWithDynamicPads)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC()DOC")
      .Attr(
          "kernel_shape",
          "",
          AttributeProto::INTS,
          OPTIONAL_VALUE)
      .Attr("output_padding",
            "",
            AttributeProto::INTS,
            OPTIONAL_VALUE)
      .Attr(
          "dilations",
          "",
          AttributeProto::INTS,
          OPTIONAL_VALUE)
      .Attr(
          "strides",
          "",
          AttributeProto::INTS,
          OPTIONAL_VALUE)
      .Attr(
          "auto_pad",
          "",
          AttributeProto::STRING,
          std::string("NOTSET"))
      .Attr(
          "group",
          "",
          AttributeProto::INT,
          static_cast<int64_t>(1))
      .Input(
          0,
          "X",
          "",
          "T")
      .Input(
          1,
          "W",
          "",
          "T")
      .Input(2, "Pads", "", "tensor(int64)", OpSchema::Optional)
      .Input(3, "B", "", "T", OpSchema::Optional)
      .Output(
          0,
          "Y",
          "",
          "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::convTransposeWithDynamicPadsShapeInference);

  ONNX_CONTRIB_OPERATOR_SCHEMA(FusedConv)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC(
The fused convolution operator schema is the same as Conv besides it includes an attribute
activation.)DOC")
      .Attr(
          "auto_pad",
          "",
          AttributeProto::STRING,
          std::string("NOTSET"))
      .Attr(
          "kernel_shape",
          "",
          AttributeProto::INTS,
          OPTIONAL_VALUE)
      .Attr(
          "dilations",
          "",
          AttributeProto::INTS,
          OPTIONAL_VALUE)
      .Attr(
          "strides",
          "",
          AttributeProto::INTS,
          OPTIONAL_VALUE)
      .Attr(
          "pads",
          "",
          AttributeProto::INTS,
          OPTIONAL_VALUE)
      .Attr(
          "group",
          "",
          AttributeProto::INT,
          static_cast<int64_t>(1))
      .Attr(
          "activation",
          "",
          AttributeProto::STRING,
          OPTIONAL_VALUE)
      .Attr(
          "activation_params",
          "",
          AttributeProto::FLOATS,
          OPTIONAL_VALUE)
      .Input(
          0,
          "X",
          "",
          "T")
      .Input(
          1,
          "W",
          "",
          "T")
      .Input(
          2,
          "B",
          "",
          "T",
          OpSchema::Optional)
      .Input(
          3,
          "Z",
          "",
          "T",
          OpSchema::Optional)
      .Output(
          0,
          "Y",
          "",
          "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)"}, "Constrain input and output types to float tensors")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 0);
        ONNX_NAMESPACE::convPoolShapeInference(ctx, true, false, 0, 1);
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(FusedGemm)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC(
The FusedGemm operator schema is the same as Gemm besides it includes attributes
activation and leaky_relu_alpha.)DOC")
      .Input(
          0,
          "A",
          "Input tensor A. "
          "The shape of A should be (M, K) if transA is 0, "
          "or (K, M) if transA is non-zero.",
          "T")
      .Input(
          1,
          "B",
          "Input tensor B. "
          "The shape of B should be (K, N) if transB is 0, "
          "or (N, K) if transB is non-zero.",
          "T")
      .Input(
          2,
          "C",
          "Input tensor C. "
          "The shape of C should be unidirectional broadcastable to (M, N).",
          "T")
      .Output(0, "Y", "Output tensor of shape (M, N).", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)",
           "tensor(float)",
           "tensor(double)",
           "tensor(uint32)",
           "tensor(uint64)",
           "tensor(int32)",
           "tensor(int64)"},
          "Constrain input and output types to float/int tensors.")
      .Attr(
          "transA",
          "Whether A should be transposed",
          AttributeProto::INT,
          static_cast<int64_t>(0))
      .Attr(
          "transB",
          "Whether B should be transposed",
          AttributeProto::INT,
          static_cast<int64_t>(0))
      .Attr(
          "alpha",
          "Scalar multiplier for the product of input tensors A * B.",
          AttributeProto::FLOAT,
          1.0f)
      .Attr(
          "beta",
          "Scalar multiplier for input tensor C.",
          AttributeProto::FLOAT,
          1.0f)
      .Attr(
          "activation",
          "",
          AttributeProto::STRING,
          OPTIONAL_VALUE)
      .Attr(
          "activation_alpha",
          "",
          AttributeProto::FLOAT,
          OPTIONAL_VALUE)
      .Attr(
          "activation_beta",
          "",
          AttributeProto::FLOAT,
          OPTIONAL_VALUE)
      .Attr(
          "activation_gamma",
          "",
          AttributeProto::FLOAT,
          OPTIONAL_VALUE)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        propagateElemTypeFromInputToOutput(ctx, 0, 0);
        if (hasNInputShapes(ctx, 2)) {
          auto transAAttr = ctx.getAttribute("transA");
          bool transA =
              transAAttr ? static_cast<int>(transAAttr->i()) != 0 : false;
          auto transBAttr = ctx.getAttribute("transB");
          bool transB =
              transBAttr ? static_cast<int>(transBAttr->i()) != 0 : false;
          auto& first_input_shape = getInputShape(ctx, 0);
          auto& second_input_shape = getInputShape(ctx, 1);
          if (first_input_shape.dim_size() != 2)
            fail_shape_inference("First input does not have rank 2");
          if (second_input_shape.dim_size() != 2)
            fail_shape_inference("Second input does not have rank 2");
          updateOutputShape(
              ctx,
              0,
              {first_input_shape.dim(transA ? 1 : 0),
               second_input_shape.dim(transB ? 0 : 1)});
        }
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(ExpandDims)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "X", "input", "T")
      .Input(1, "axis", "Specified axis to insert a dimension", "tensor(int32)")
      .Output(0, "Y", "output", "T")
      .TypeConstraint(
          "T",
          ONNX_NAMESPACE::OpSchema::all_tensor_types(),
          "Constrain to any tensor type. If the dtype attribute is not provided this must be a valid output type.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        // Type inference
        propagateElemTypeFromInputToOutput(ctx, 0, 0);

        // Shape inference
        if (!hasInputShape(ctx, 0))
          return;

        auto& input_shape = getInputShape(ctx, 0);
        const int rank = input_shape.dim_size();
        const ONNX_NAMESPACE::TensorProto* axis_initializer = ctx.getInputData(1);
        if (!axis_initializer)
          return;
        const int axis = axis_initializer->int32_data()[0];
        if (axis > rank || axis < -rank - 1) {
          fail_shape_inference("Input axis is invalid: ", axis);
        }
        int pos = axis >= 0 ? axis : rank + axis - 1;
        ONNX_NAMESPACE::TensorShapeProto output_shape;
        for (int i = 0; i < pos; ++i) {
          output_shape.add_dim();
          *(output_shape.mutable_dim(i)) = input_shape.dim(i);
        }
        output_shape.add_dim();
        output_shape.mutable_dim(pos)->set_dim_value(1);
        for (int i = pos + 1; i < rank + 1; ++i) {
          output_shape.add_dim();
          *(output_shape.mutable_dim(i)) = input_shape.dim(i - 1);
        }
        updateOutputShape(ctx, 0, output_shape);
      })
      .SetDoc(R"DOC(ExpandDims echo operator.)DOC");

  ONNX_CONTRIB_OPERATOR_SCHEMA_ELSEWHERE(AttnLSTM, RegisterAttnLSTMContribOpSchema);
  ONNX_CONTRIB_OPERATOR_SCHEMA_ELSEWHERE(Range, RegisterRangeOpSchema);

  static const char* Tokenizer_ver1_doc = R"DOC(
  Tokenizer divides each string in X into a vector of strings along the last axis. Allowed input shapes are [C] and [N, C].
  If the maximum number of tokens found per input string is D, the output shape would be [N, C, D] when input shape is [N, C].
  Similarly, if input shape is [C] then the output should be [C, D]. Tokenizer has two different operation modes.
  The first mode is selected when "tokenexp" is not set and "separators" is set. If "tokenexp" is set and "separators" is not set,
  the second mode will be used. The first mode breaks each input string into tokens by matching and removing separators.
  "separators" is a list of strings which are regular expressions. "tokenexp" is a single regular expression.
  Let's assume "separators" is [" "] and consider an example.
  If input is
  ["Hello World", "I love computer science !"] whose shape is [2],
  then the output would be
 [["Hello", "World", padvalue, padvalue, padvalue],
 ["I", "love", "computer", "science", "!"]]
 whose shape is [2, 5] because you can find at most 5 tokens per input string.
 Note that the input at most can have two axes, so 3-D and higher dimension are not supported.
 If "separators" contains a single empty string, the Tokenizer will enter into character tokenezation mode. This means all strings
 will be broken part into individual characters.
 For each input string, the second mode searches matches of "tokenexp" and each match will be a token in Y.
 The matching of "tokenexp" is conducted greedily (i.e., a match should be as long as possible).
 This operator searches for the first match starting from the beginning of the considered string,
 and then launches another search starting from the first remained character after the first matched token.
 If no match found, this operator will remove the first character from the remained string and do another search.
 This procedure will be repeated until reaching the end of the considered string.
  Let's consider another example to illustrate the effect of setting "mark" to true.
  If input is ["Hello", "World"],
  then the corresponding output would be [0x02, "Hello", "World", 0x03].
  This implies that if mark is true, [C]/[N, C] - input's output shape becomes [C, D+2]/[N, C, D+2].
If tokenizer removes the entire content of [C]-input, it will produce [[]].
I.e. the output shape should be [C][0] or [N][C][0] if input shape was [N][C].
If the tokenizer receives empty input of [0] then the output is [0] if empty input
of [N, 0] then [N, 0].
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(Tokenizer)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "X", "Strings to tokenize", "T")
      .Output(0, "Y", "Tokenized strings", "T")
      .TypeConstraint(
          "T",
          {"tensor(string)"},
          "Input/Output is a string tensor")
      .Attr(
          "mark",
          "Boolean whether to mark the beginning/end character with start of text character (0x02)/end of text character (0x03).",
          AttributeProto::INT)
      .Attr(
          "pad_value",
          "The string used to pad output tensors when the tokens extracted doesn't match the maximum number of tokens found. If start/end markers are needed, padding will appear outside the markers.",
          AttributeProto::STRING)
      .Attr(
          "tokenexp",
          "An optional string. Token's regular expression in basic POSIX format"
          " (pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap09.html#tag_09_03)."
          " If set, tokenizer may produce tokens matching the specified pattern. Note that one and only of"
          " 'tokenexp' and 'separators' should be set.",
          AttributeProto::STRING,
          OPTIONAL_VALUE)
      .Attr(
          "separators",
          "an optional list of strings attribute that contains a list of separators - regular expressions to match separators"
          " Two consecutive segments in X connected by a separator would be divided into two tokens."
          " For example, if the input is \"Hello World!\" and this attribute contains only one space character,"
          " the corresponding output would be [\"Hello\", \"World!\"]. To achieve character-level tokenization,"
          " one should set the 'separators' to [\"\"], which contains an empty string.",
          AttributeProto::STRINGS,
          OPTIONAL_VALUE)
      .Attr(
          "mincharnum",
          "Minimum number of characters allowed in the output. For example, if mincharnum is 2, tokens such as \"A\" and \"B\" would be ignored",
          AttributeProto::INT)
      .SetDoc(Tokenizer_ver1_doc)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        propagateElemTypeFromInputToOutput(ctx, 0, 0);

        // Shape inference
        if (!hasInputShape(ctx, 0))
          return;

        ONNX_NAMESPACE::TensorShapeProto output_shape;
        auto& input_shape = getInputShape(ctx, 0);
        auto& dims = input_shape.dim();
        if (dims.size() < 1 || dims.size() > 2) {
          fail_shape_inference("Input dimensions are either [C] or [N][C] allowed");
        }

        int64_t size = 1;
        for (auto& dim : dims) {
          if (utils::HasDimValue(dim)) {
            size *= dim.dim_value();
          }
        }

        if (size > 0) {
          for (auto& dim : dims) {
            *output_shape.add_dim() = dim;
          }
          // Add the last unknown dimension
          // only if the input is not empty
          output_shape.add_dim();
        } else if (size == 0) {
          if (dims.size() == 2) {
            *output_shape.add_dim() = dims[0];
          }
          output_shape.add_dim()->set_dim_value(0);
        }
        updateOutputShape(ctx, 0, output_shape);
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(MatMulInteger16)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC(
Matrix product that behaves like numpy.matmul: https://docs.scipy.org/doc/numpy-1.13.0/reference/generated/numpy.matmul.html.
 The production MUST never overflow. The accumulation may overflow if and only if in 32 bits.)DOC")
      .Input(0, "A", "N-dimensional matrix A", "T1")
      .Input(1, "B", "N-dimensional matrix B", "T2")
      .Output(0, "Y", "Matrix multiply results from A * B", "T3")
      .TypeConstraint("T1", {"tensor(int16)", "tensor(uint16)"}, "Constrain input A data types as 16-bit integer tensor")
      .TypeConstraint("T2", {"tensor(int16)", "tensor(uint16)"}, "Constrain input B data types as 16-bit integer tensor")
      .TypeConstraint("T3",
                      {"tensor(int32)", "tensor(uint32)"},
                      "Constrain output Y data types as 32-bit integer tensor."
                      "T3 must be tensor(uint32) when both T1 and T2 are tensor(uint16),"
                      "or must be tensor(int32) when either T1 or T2 is tensor(int16).")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        auto a_type = ctx.getInputType(0);
        auto b_type = ctx.getInputType(1);
        auto y_type = ctx.getOutputType(0);
        if (nullptr == a_type || nullptr == b_type || nullptr == y_type ||
            a_type->value_case() != ONNX_NAMESPACE::TypeProto::kTensorType ||
            b_type->value_case() != ONNX_NAMESPACE::TypeProto::kTensorType) {
          fail_type_inference(
              "inputs are expected to have tensor type and output type should not be null.");
        }

        // Right now we only support int32
        y_type->mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto::INT32);

        ONNX_NAMESPACE::matmulShapeInference(ctx, 0, 1);
      });

  static const char* TransposeMatMul_doc = R"DOC(
Duplicate of FusedMatMul. Going forward FusedMatMul should be used. This OP will be supported for backward compatibility.
Matrix product that behaves like numpy.matmul: https://docs.scipy.org/doc/numpy-1.13.0/reference/generated/numpy.matmul.html
)DOC";

  static const char* FusedMatMul_doc = R"DOC(
Matrix product that behaves like numpy.matmul: https://docs.scipy.org/doc/numpy-1.13.0/reference/generated/numpy.matmul.html
)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(TransposeMatMul)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "A", "N-dimensional matrix A", "T")
      .Input(1, "B", "N-dimensional matrix B", "T")
      .Attr(
          "alpha",
          "Scalar multiplier for the product of the input tensors.",
          AttributeProto::FLOAT,
          1.0f)
      .Attr(
          "transA",
          "Whether A should be transposed on the last two dimensions before doing multiplication",
          AttributeProto::INT,
          static_cast<int64_t>(0))
      .Attr(
          "transB",
          "Whether B should be transposed on the last two dimensions before doing multiplication",
          AttributeProto::INT,
          static_cast<int64_t>(0))
      .Output(0, "Y", "Matrix multiply results", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
          "Constrain input and output types to float tensors.")
      .SetDoc(TransposeMatMul_doc)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        FusedMatMulShapeInference(ctx);
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(FusedMatMul)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "A", "N-dimensional matrix A", "T")
      .Input(1, "B", "N-dimensional matrix B", "T")
      .Attr("alpha", "Scalar multiplier for the product of the input tensors.", AttributeProto::FLOAT, 1.0f)
      .Attr("transA", "Whether A should be transposed on the last two dimensions before doing multiplication",
            AttributeProto::INT, static_cast<int64_t>(0))
      .Attr("transB", "Whether B should be transposed on the last two dimensions before doing multiplication",
            AttributeProto::INT, static_cast<int64_t>(0))
      .Attr("transBatchA",
            "Whether A should be transposed on the 1st dimension and batch dimensions (dim-1 to dim-rank-2) before "
            "doing multiplication",
            AttributeProto::INT, static_cast<int64_t>(0))
      .Attr("transBatchB",
            "Whether B should be transposed on the 1st dimension and batch dimensions (dim-1 to dim-rank-2) before "
            "doing multiplication",
            AttributeProto::INT, static_cast<int64_t>(0))
      .Output(0, "Y", "Matrix multiply results", "T")
      .TypeConstraint("T", {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
                      "Constrain input and output types to float tensors.")
      .SetDoc(FusedMatMul_doc)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) { FusedMatMulShapeInference(ctx); });

  ONNX_CONTRIB_OPERATOR_SCHEMA(SparseToDenseMatMul)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "A", "2-dimensional sparse matrix A. Either COO or CSR format", "T")
      .Input(1, "B", "N-dimensional dense matrix B", "T1")
      .Attr(
          "alpha",
          "Scalar multiplier for the product of the input tensors.",
          AttributeProto::FLOAT,
          1.0f)
      .Attr(
          "transA",
          "Whether A should be transposed on the last two dimensions before doing multiplication",
          AttributeProto::INT,
          static_cast<int64_t>(0))
      .Attr(
          "transB",
          "Whether B should be transposed on the last two dimensions before doing multiplication",
          AttributeProto::INT,
          static_cast<int64_t>(0))
      .Output(0, "Y", "Matrix multiply results", "T1")
      .TypeConstraint(
          "T",
          {"sparse_tensor(float)", "sparse_tensor(double)", "sparse_tensor(int64)", "sparse_tensor(int32)",
           "sparse_tensor(uint64)", "sparse_tensor(uint32)"},
          "Constrain input and output types to float tensors.")
      .TypeConstraint(
          "T1",
          {"tensor(float)", "tensor(double)", "tensor(int64)", "tensor(int32)",
           "tensor(uint64)", "tensor(uint32)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        // 1- dense tensor to output
        propagateElemTypeFromInputToOutput(ctx, 1, 0);
        // TODO: replace with ONNX one when that one is fixed
        sparseCompatibleMatmulShapeInference(ctx, 0, 1);
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(MurmurHash3)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(R"DOC(The underlying implementation is MurmurHash3_x86_32 generating low latency 32bits hash suitable for implementing lookup tables, Bloom filters, count min sketch or feature hashing.)DOC")
      .Input(0, "X", "An input tensor to hash.", "T1")
      .Output(0, "Y", "32-bit hash value.", "T2")
      .TypeConstraint("T1", {"tensor(uint32)", "tensor(int32)", "tensor(uint64)", "tensor(int64)", "tensor(float)", "tensor(double)", "tensor(string)"}, "Constrain input type to unsigned or signed 32-bit integer tensor, or string tensor. It should be utf-8 encoded if using unicode.")
      .TypeConstraint("T2", {"tensor(uint32)", "tensor(int32)"}, "Constrain output type to unsigned and signed 32-bit integer tensor.")
      .Attr(
          "seed",
          "Seed for the hashing algorithm, unsigned 32-bit integer, default to 0.",
          AttributeProto::INT,
          (int64_t)0LL)
      .Attr(
          "positive",
          "If value is 1, output type is uint32_t, else int32_t. Default value is 1.",
          AttributeProto::INT,
          (int64_t)1LL)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        // type inference
        auto positive_attr = ctx.getAttribute("positive");
        bool is_positive =
            positive_attr ? (static_cast<int>(positive_attr->i()) == 1 ? true : false) : true /* default value if attribute not present */;
        auto output_data_type = ctx.getOutputType(0)->mutable_tensor_type();
        if (is_positive) {
          output_data_type->set_elem_type(::ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT32);
        } else {
          output_data_type->set_elem_type(::ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT32);
        }

        // Shape inference
        if (!hasInputShape(ctx, 0))
          return;

        auto& input_shape = getInputShape(ctx, 0);
        updateOutputShape(ctx, 0, input_shape);
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(GatherND)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "data", "Tensor of rank r >= 1.", "T")
      .Input(1, "indices", "Tensor of rank q >= 1.", "Tind")
      .Output(0, "output", "Tensor of rank q-1+r-indices[-1].", "T")
      .TypeConstraint(
          "T",
          OpSchema::all_tensor_types(),
          "Constrain input and output types to any tensor type.")
      .TypeConstraint(
          "Tind",
          {"tensor(int32)", "tensor(int64)"},
          "Constrain indice type to int32 or int64")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        propagateElemTypeFromInputToOutput(ctx, 0, 0);
        if (!hasNInputShapes(ctx, 2)) {
          return;
        }
        auto& data_shape = ctx.getInputType(0)->tensor_type().shape();
        auto& indices_shape = ctx.getInputType(1)->tensor_type().shape();
        auto data_rank = data_shape.dim_size();
        auto indices_rank = indices_shape.dim_size();
        if (data_rank < 1 || indices_rank < 1) {
          fail_shape_inference("both data and indices tensor need to have rank larger than zero.");
        }
        auto last_indice_dimension = indices_shape.dim(indices_rank - 1).dim_value();
        if (last_indice_dimension > data_rank) {
          fail_shape_inference("last dimension of indices must not be larger and rank of data tensor");
        }
        for (int i = 0; i < indices_rank - 1; ++i) {
          *ctx.getOutputType(0)
               ->mutable_tensor_type()
               ->mutable_shape()
               ->add_dim() = indices_shape.dim(i);
        }
        for (int i = static_cast<int>(last_indice_dimension); i < data_rank; ++i) {
          *ctx.getOutputType(0)
               ->mutable_tensor_type()
               ->mutable_shape()
               ->add_dim() = data_shape.dim(i);
        }
      })
      .SetDoc(R"DOC(
Given `data` tensor of rank r >= 1, and `indices` tensor of rank q >= 1, gather
slices of `data` into an output tensor of rank q - 1 + r - indices[-1].
Example 1:
  data    = [[0,1],[2,3]]
  indices = [[0,0],[1,1]]
  output  = [0,3]
Example 2:
  data    = [[0,1],[2,3]]
  indices = [[1],[0]]
  output  = [[2,3],[0,1]]
Example 3:
  data    = [[[0,1],[2,3]],[[4,5],[6,7]]]
  indices = [[0,1],[1,0]]
  output  = [[2,3],[4,5]]
Example 4:
  data    = [[[0,1],[2,3]],[[4,5],[6,7]]]
  indices = [[[0,1]],[[1,0]]]
  output  = [[[2,3]],[[4,5]]]
)DOC");

  ONNX_CONTRIB_OPERATOR_SCHEMA(WordConvEmbedding)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Attr(
          "embedding_size",
          "Integer representing the embedding vector size for each word."
          "If not provide, use the fileter size of conv weight",
          AttributeProto::INT,
          OPTIONAL_VALUE)
      .Attr(
          "conv_window_size",
          "This operator applies convolution to word from left to right with window equal to conv_window_size and stride to 1."
          "Take word 'example' for example, with conv_window_size equal to 2, conv is applied to [ex],[xa], [am], [mp]..."
          "If not provide, use the first dimension of conv kernal shape.",
          AttributeProto::INT,
          OPTIONAL_VALUE)
      .Attr(
          "char_embedding_size",
          "Integer representing the embedding vector size for each char."
          "If not provide, use the char embedding size of embedding vector.",
          AttributeProto::INT,
          OPTIONAL_VALUE)
      .Input(0, "Sequence", "Specify batchs of sequence words to embedding", "T")
      .Input(1, "W", "Specify weights of conv", "T1")
      .Input(2, "B", "Specify bias of conv", "T1")
      .Input(3, "C", "Specify embedding vector of char", "T1")
      .Output(0, "Y", "output", "T1")
      .TypeConstraint(
          "T",
          {"tensor(int32)"},
          "Constrain to tensor(int32).")
      .TypeConstraint(
          "T1",
          {"tensor(float)"},
          "Constrain to tensor(float).")
      .SetDoc(R"DOC(The WordConvEmbedding takes in a batch of sequence words and embed each word to a vector.)DOC");

  ONNX_CONTRIB_OPERATOR_SCHEMA(Pad)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Attr(
          "mode",
          "Three modes: `constant`(default) - pads with a given constant value, "
          "`reflect` - pads with the reflection of the vector mirrored on the first and last values of the vector along each axis, "
          "`edge` - pads with the edge values of array",
          AttributeProto::STRING,
          std::string("constant"))
      .Input(0, "data", "Input tensor.", "T")
      .Input(
          1,
          "pads",
          "Tensor of integers indicating the number of padding elements to add or remove (if negative) "
          "at the beginning and end of each axis. For 2D input tensor, it is the number of pixels. "
          "`pads` should be a 1D tensor of shape [2 * input_rank] or a 2D tensor of shape [1, 2 * input_rank]. "
          "`pads` format (1D example) should be as follow [x1_begin, x2_begin,...,x1_end, x2_end,...], "
          "where xi_begin is the number of pixels added at the beginning of axis `i` and "
          "xi_end, the number of pixels added at the end of axis `i`.",
          "tensor(int64)")
      .Input(
          2,
          "value",
          "(Optional) A scalar or rank 1 tensor containing a single value to be filled if the mode chosen is `constant` (by default it is 0.0).",
          "T",
          OpSchema::Optional)
      .Output(0, "output", "Tensor after padding.", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        // Type inference
        propagateElemTypeFromInputToOutput(ctx, 0, 0);
        // Shape inference needs the input data shape
        if (!hasNInputShapes(ctx, 1)) {
          return;
        }
        const auto& input_shape = ctx.getInputType(0)->tensor_type().shape();
        const auto input_rank = input_shape.dim_size();

        // Infer output shape if 'pads' tensor is available
        const auto* pads_initializer = ctx.getInputData(1);
        if (nullptr != pads_initializer) {
          const auto& pads_shape = ctx.getInputType(1)->tensor_type().shape();
          if ((pads_initializer->dims_size() != 1 &&
               pads_initializer->dims_size() != 2) ||
              (pads_initializer->dims_size() == 2 &&
               pads_shape.dim(static_cast<int>(0)).dim_value() != 1) ||
              pads_initializer->data_type() != ONNX_NAMESPACE::TensorProto::INT64)
            fail_shape_inference(
                "'pads' input must be a 1D (shape: [input_rank]) "
                "or 2D tensor (shape: [1, input_rank]) of type int64");

          // make a copy of the returned const vector - may have to resize
          // this in next step
          std::vector<int64_t> pads_data;
          if (utils::HasRawData(*pads_initializer))
            return;
          else
            pads_data.insert(
                pads_data.end(),
                pads_initializer->int64_data().begin(),
                pads_initializer->int64_data().end());

          // fill with zeros if needed to reach appropriate size
          if (pads_data.size() != 2 * static_cast<size_t>(input_rank))
            pads_data.resize(size_t{2} * input_rank, 0);

          const auto& output_shape =
              ctx.getOutputType(0)->mutable_tensor_type()->mutable_shape();
          for (size_t i = 0; static_cast<int64_t>(i) < input_rank; ++i) {
            const auto& input_dim = input_shape.dim(static_cast<int>(i));
            auto* output_dim = output_shape->add_dim();
            if (utils::HasDimValue(input_dim)) {
              output_dim->set_dim_value(
                  input_dim.dim_value() + pads_data[i] + pads_data[i + input_rank]);
            } else if (pads_data[i] + pads_data[i + input_rank] == 0) {
              *output_dim = input_dim;
            }
          }
        } else {
          // Infer output shapes' rank in any case
          auto* output_shape_0 = getOutputShape(ctx, 0);
          for (size_t i = 0; static_cast<int64_t>(i) < input_rank; ++i) {
            output_shape_0->add_dim();
          }
        }
        return;
      })
      .SetDoc(R"DOC(
            Given `data` tensor, pads, mode, and value.
            Example:
            Insert 0 pads to the beginning of the second dimension.
            data = [
                    [1.0, 1.2],
                    [2.3, 3.4],
                    [4.5, 5.7],
                    ]
            pads = [0, 2, 0, 0]
            output = [
                    [
                    [0.0, 0.0, 1.0, 1.2],
                    [0.0, 0.0, 2.3, 3.4],
                    [0.0, 0.0, 4.5, 5.7],
                    ],
                    ]
            )DOC");

  ONNX_CONTRIB_OPERATOR_SCHEMA(Unique)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "x", "A 1-D input tensor that is to be processed.", "T")
      .Output(0, "y",
              "A 1-D tensor of the same type as 'x' "
              "containing all the unique values in 'x' sorted "
              "in the same order that they occur in the input 'x'",
              "T")
      .Output(1, "idx",
              "A 1-D INT64 tensor of the same size as 'x' "
              "containing the indices for each value in 'x' "
              "in the output 'uniques'",
              "tensor(int64)")
      .Output(2, "counts",
              "A 1-D INT64 tensor containing the "
              "the count of each element "
              "of 'uniques' in the input 'x'",
              "tensor(int64)")
      .TypeConstraint("T", OpSchema::all_tensor_types(), "Input can be of any tensor type.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        // Type inference
        ONNX_NAMESPACE::propagateElemTypeFromInputToOutput(ctx, 0, 0);
        ONNX_NAMESPACE::updateOutputElemType(ctx, 1, ONNX_NAMESPACE::TensorProto::INT64);
        ONNX_NAMESPACE::updateOutputElemType(ctx, 2, ONNX_NAMESPACE::TensorProto::INT64);

        // Shape inference

        // shape of output 'uniques' and 'counts'
        // depends on actual input data, but the rank is always 1
        ctx.getOutputType(0)
            ->mutable_tensor_type()
            ->mutable_shape()
            ->add_dim();

        ctx.getOutputType(2)
            ->mutable_tensor_type()
            ->mutable_shape()
            ->add_dim();

        // if the input shape doesn't exist, further shape inference is not possible
        if (!hasNInputShapes(ctx, 1)) {
          return;
        }

        // 'idx' output has same shape as input
        ONNX_NAMESPACE::propagateShapeFromInputToOutput(ctx, 0, 1);

        return;
      })
      .SetDoc(R"DOC(
              Finds all the unique values (deduped list) present in the given input tensor.
              This operator returns 3 outputs.
              The first output tensor 'uniques' contains all of the unique elements of the input,
              sorted in the same order that they occur in the input.
              The second output tensor 'idx' is the same size as the input and it contains the index
              of each value of the input in 'uniques'.
              The third output tensor 'counts' contains the count of each element of 'uniques' in the input.
              Example:
                input_x = [2, 1, 1, 3, 4, 3]
                output_uniques = [2, 1, 3, 4]
                output_idx = [0, 1, 1, 2, 3, 2]
                output_counts = [1, 2, 2, 1]
              )DOC");

  //see:https://docs.scipy.org/doc/scipy/reference/generated/scipy.spatial.distance.cdist.html
  ONNX_CONTRIB_OPERATOR_SCHEMA(CDist)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Attr("metric",
            "The distance metric to use. If a string, the distance function can be \"braycurtis\", \"canberra\", "
            "\"chebyshev\", \"cityblock\", \"correlation\", \"cosine\", \"dice\", \"euclidean\", \"hamming\", \"jaccard\", "
            "\"jensenshannon\", \"kulsinski\", \"mahalanobis\", \"matching\", \"minkowski\", \"rogerstanimoto\", \"russellrao\", "
            "\"seuclidean\", \"sokalmichener\", \"sokalsneath\", \"sqeuclidean\", \"wminkowski\", \"yule\".",
            AttributeProto::STRING, std::string("sqeuclidean"))
      .Input(0, "A", "2D matrix with shape (M,N)", "T")
      .Input(1, "B", "2D matrix with shape (K,N)", "T")
      .Output(0, "C",
              "A 2D Matrix that represents the distance between each pair of the two collections of inputs.",
              "T")
      .TypeConstraint("T", {"tensor(float)", "tensor(double)"}, "Constrains input to only numeric types.");

  ONNX_CONTRIB_OPERATOR_SCHEMA(CropAndResize)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Attr(
          "mode",
          "The pooling method. Two modes are supported: 'bilinear' and 'nearest'. "
          "Default is 'bilinear'.",
          AttributeProto::STRING,
          std::string("bilinear"))
      .Attr(
          "extrapolation_value",
          "Value used for extrapolation, when applicable. "
          "Default is 0.0f. ",
          AttributeProto::FLOAT,
          0.f)
      .Input(
          0,
          "X",
          "Input data tensor from the previous operator; "
          "4-D feature map of shape (N, C, H, W), "
          "where N is the batch size, C is the number of channels, "
          "and H and W are the height and the width of the data.",
          "T1")
      .Input(
          1,
          "rois",
          "RoIs (Regions of Interest) to pool over; rois is "
          "2-D input of shape (num_rois, 4) given as "
          "[[y1, x1, y2, x2], ...]. "
          "The RoIs' coordinates are normalized in the coordinate system of the input image. "
          "Each coordinate set has a 1:1 correspondence with the 'batch_indices' input.",
          "T1")
      .Input(
          2,
          "batch_indices",
          "1-D tensor of shape (num_rois,) with each element denoting "
          "the index of the corresponding image in the batch.",
          "T2")
      .Input(
          3,
          "crop_size",
          "1-D tensor of 2 elements: [crop_height, crop_width]. "
          "All cropped image patches are resized to this size. Both crop_height and crop_width need to be positive.",
          "T2")
      .Output(
          0,
          "Y",
          "RoI pooled output, 4-D tensor of shape "
          "(num_rois, C, crop_height, crop_width). The r-th batch element Y[r-1] "
          "is a pooled feature map corresponding to the r-th RoI X[r-1].",
          "T1")
      .TypeConstraint(
          "T1",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain types to float tensors.")
      .TypeConstraint(
          "T2",
          {"tensor(int32)"},
          "Constrain types to int tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        if (!hasNInputShapes(ctx, 4)) {
          return;
        }
        propagateElemTypeFromInputToOutput(ctx, 0, 0);

        auto& input_shape = getInputShape(ctx, 0);
        auto& rois_shape = getInputShape(ctx, 1);
        auto& batch_index_shape = getInputShape(ctx, 2);
        auto& crop_size_shape = getInputShape(ctx, 3);

        if (input_shape.dim_size() != 4) {
          fail_shape_inference("first input tensor has wrong dimension");
        }
        if (rois_shape.dim_size() != 2) {
          fail_shape_inference("rois input tensor has wrong dimension");
        }
        if (batch_index_shape.dim_size() != 1) {
          fail_shape_inference("batch_indices shape input tensor has wrong dimension");
        }
        if (crop_size_shape.dim_size() != 1) {
          fail_shape_inference("crop_size shape input tensor has wrong dimension");
        }
      })
      .SetDoc(R"DOC(
        Extracts crops from the input image tensor and resizes them using bilinear sampling or nearest neighbor sampling
        (possibly with aspect ratio change) to a common output size specified by crop_height and crop_width.
        Returns a tensor with crops from the input image at positions defined at the bounding box locations in boxes.
        The cropped boxes are all resized (with bilinear or nearest neighbor interpolation) to
        a fixed size = [crop_height, crop_width]. The result is a 4-D tensor [num_boxes, crop_height, crop_width, depth].
        The resizing is corner aligned.)DOC");

  ONNX_CONTRIB_OPERATOR_SCHEMA(LayerNormalization)
      .SetDomain(kOnnxDomain)
      .SinceVersion(1)
      .SetSupportLevel(OpSchema::SupportType::EXPERIMENTAL)
      .SetDoc("LayerNormalization")
      .Attr("axis",
            "The first normalization dimension: normalization will be performed along dimensions axis : rank(inputs).",
            AttributeProto::INT, static_cast<int64_t>(-1))
      .Attr("epsilon",
            "The epsilon value to use to avoid division by zero.",
            AttributeProto::FLOAT, 1e-5f)
      .Attr("stash_type",
            "type used for stash mean/inv_std_var",
            AttributeProto::INT, static_cast<int64_t>(ONNX_NAMESPACE::TensorProto_DataType_FLOAT))
      .AllowUncheckedAttributes()
      .Input(0, "X", "Input data tensor from the previous layer.", "T")
      .Input(1, "Scale", "Scale tensor.", "T")
      .Input(2, "B", "Bias tensor.", "T", OpSchema::Optional)
      .Output(0, "Y", "Output data tensor.", "T")
      .Output(1, "Mean", "Saved mean used during training to speed up gradient computation", "U", OpSchema::Optional)
      .Output(2, "InvStdDev", "Saved inverse standard deviation used during training to speed up gradient computation.", "U", OpSchema::Optional)
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
          "Constrain input types and output Y type to float tensors.")
      .TypeConstraint(
          "U",
          {"tensor(float)", "tensor(bfloat16)"},
          "Type of Mean and InvStdDev tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        propagateShapeAndTypeFromFirstInput(ctx);
        propagateElemTypeFromInputToOutput(ctx, 0, 0);
        auto type = ctx.getAttribute("stash_type")->i();
        if (ctx.getNumOutputs() > 1) {
          auto output_type = ctx.getOutputType(1);
          output_type->mutable_tensor_type()->set_elem_type(static_cast<int32_t>(type));
        }
        if (ctx.getNumOutputs() > 2) {
          auto output_type = ctx.getOutputType(2);
          output_type->mutable_tensor_type()->set_elem_type(static_cast<int32_t>(type));
        }
        if (!hasNInputShapes(ctx, 1)) {
          return;
        }
        auto& input_shape = ctx.getInputType(0)->tensor_type().shape();
        int64_t input_ndim = input_shape.dim_size();
        int64_t axis = -1;
        auto axis_proto = ctx.getAttribute("axis");
        if (axis_proto) {
          axis = axis_proto->i();
        }
        if (axis < 0) {
          axis += input_ndim;
        }

        if (ctx.getNumOutputs() > 1) {
          auto saved_mean_shape = ctx.getOutputType(1)->mutable_tensor_type()->mutable_shape();
          saved_mean_shape->CopyFrom(input_shape);
          for (int d = static_cast<int>(axis); d < input_ndim; ++d)
            saved_mean_shape->mutable_dim(d)->set_dim_value(1);
        }

        if (ctx.getNumOutputs() > 2) {
          auto saved_inv_std_dev_shape = ctx.getOutputType(2)->mutable_tensor_type()->mutable_shape();
          saved_inv_std_dev_shape->CopyFrom(input_shape);
          for (int d = static_cast<int>(axis); d < input_ndim; ++d)
            saved_inv_std_dev_shape->mutable_dim(d)->set_dim_value(1);
        }
      })
      .SetContextDependentFunctionBodyBuilder(
          [](const FunctionBodyBuildContext& ctx, const OpSchema& schema, FunctionProto& functionProto) {
            // LayerNormalization <axis, epsilon, stash_type> (X, Scale, B) => (Y, Mean?, InvStdDev?)

            auto* tp = ctx.getInputType(0);
            if ((tp == nullptr) || (!tp->has_tensor_type()))
              return false;
            int64_t T = tp->tensor_type().elem_type();

            auto type_attr = ctx.getAttribute("stash_type");
            int64_t U = (type_attr != nullptr) ? type_attr->i() : static_cast<int64_t>(ONNX_NAMESPACE::TensorProto_DataType_FLOAT);
            if ((U != ONNX_NAMESPACE::TensorProto_DataType_FLOAT) && (U != ONNX_NAMESPACE::TensorProto_DataType_BFLOAT16))
              return false;  // Error

            auto* axis_attr = ctx.getAttribute("axis");
            int64_t axis = (axis_attr != nullptr) ? axis_attr->i() : -1;
            auto* epsilon_attr = ctx.getAttribute("epsilon");
            float epsilon = (epsilon_attr != nullptr) ? epsilon_attr->f() : 1e-5f;

            auto mktensor = [](int64_t val) -> ONNX_NAMESPACE::TensorProto {
              auto tp = ONNX_NAMESPACE::ToTensor(std::vector<int64_t>{val});
              tp.add_dims(1);
              return tp;
            };

            // The treatment of "axis" is different in "LayerNormalization" and in Reduction operations.
            // This complicates the function definition, requiring reshaping inputs/outputs.
            // Input X shape: [d[0], ..., d[axis-1], d[axis], ..., d[rank-1]]
            // This is treated as a 2D shape [d[0] * ... * d[axis-1], d[axis] * ... * d[rank-1]]
            // Normalization is applied to the second dimension.
            // Output Y has same shape as X
            // Outputs Mean and InvStdDev have shape: [d[0], ..., d[axis-1], 1, ..., 1]
            FunctionBuilder builder(functionProto);
            builder
                .AddOpset("", 13)
                .Const("Epsilon", epsilon, U)
                .Add("XShape = Shape (X)")                                                    // shape of input tensor: 1D tensor
                .Add("Rank = Size (XShape)")                                                  // rank of input tensor: scalar
                .Add("Zero1D = Constant()", "value", mktensor(0))                             // [0] : 1D tensor
                .Add("Axis1D = Constant()", "value", mktensor(axis))                          // [axis] : 1D tensor
                .Add("PrefixShape = Slice (XShape, Zero1D, Axis1D)")                          // [d[0], ..., d[axis-1]]
                .Add(axis > 0                                                                 // number of axes that are reduced =
                         ? "NumReducedAxes = Sub (Rank, Axis1D)"                              // [rank - axis]: 1D tensor
                         : "NumReducedAxes = Neg (Axis1D)")                                   // [-axis] : 1D tensor
                .Add("SuffixShape = ConstantOfShape (NumReducedAxes)", "value", mktensor(1))  // [1, ..., 1] for reduced axes
                .Add("ReducedShape = Concat <axis = 0> (PrefixShape, SuffixShape)")           // [d[0], ..., d[axis-1], 1, ..., 1]
                .Add("X2D = Flatten (X)", "axis", axis)
                .Add("XU = Cast (X2D)", "to", U)
                .Add("Mean2D = ReduceMean <axes = [1]> (XU)")
                .Add("Square = Mul (XU, XU)")
                .Add("MeanOfSquare = ReduceMean <axes = [1]> (Square)")
                .Add("SquareOfMean = Mul (Mean2D, Mean2D)")
                .Add("Var = Sub (MeanOfSquare, SquareOfMean)")
                .Add("VarPlusEpsilon = Add (Var, Epsilon)")
                .Add("StdDev = Sqrt (VarPlusEpsilon)")
                .Add("Deviation = Sub (XU, Mean2D)")
                .Add("Normalized = Div (Deviation, StdDev)")
                .Add("NormalizedT = Cast (Normalized)", "to", T)
                .Add("Scale2D = Flatten <axis = 0> (Scale)")
                .Add("Scaled = Mul (NormalizedT, Scale2D)");
            if (ctx.hasInput(2)) {
              builder.Add("B2D = Flatten <axis=0> (B)");
              builder.Add("Biased = Add (Scaled, B2D)");
            } else {
              builder.Add("Biased = Identity (Scaled)");
            }
            builder.Add("Y = Reshape (Biased, XShape)");
            builder.Add("InvStdDev2D = Reciprocal (StdDev)");
            if (ctx.hasOutput(1))
              builder.Add("Mean = Reshape (Mean2D, ReducedShape)");
            if (ctx.hasOutput(2))
              builder.Add("InvStdDev = Reshape (InvStdDev2D, ReducedShape)");

            schema.BuildFunction(functionProto);
            return true;
          });

  ONNX_CONTRIB_OPERATOR_SCHEMA(SimplifiedLayerNormalization)
      .SetDomain(kOnnxDomain)
      .SinceVersion(1)
      .SetSupportLevel(OpSchema::SupportType::EXPERIMENTAL)
      .SetDoc("SimplifiedLayerNormalization")
      .Attr("axis",
            "The first normalization dimension: normalization will be performed along dimensions axis : rank(inputs).",
            AttributeProto::INT, static_cast<int64_t>(-1))
      .Attr("epsilon",
            "The epsilon value to use to avoid division by zero.",
            AttributeProto::FLOAT, 1e-5f)
      .AllowUncheckedAttributes()
      .Input(0, "X", "Input data tensor from the previous layer.", "T")
      .Input(1, "scale", "Scale tensor.", "T")
      .Output(0, "Y", "Output data tensor.", "T")
      .Output(1, "inv_std_var", "Saved inverse standard variance used during training to speed up gradient computation.", "U", OpSchema::Optional)
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
          "Constrain input and output types (except mean and inv_std_var) to float tensors.")
      .TypeConstraint(
          "U",
          {"tensor(float)"},
          "Constrain mean and inv_std_var to be float tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        propagateShapeAndTypeFromFirstInput(ctx);
        propagateElemTypeFromInputToOutput(ctx, 0, 0);
        if (!hasNInputShapes(ctx, 1)) {
          return;
        }
        auto& input_shape = ctx.getInputType(0)->tensor_type().shape();
        int64_t input_ndim = input_shape.dim_size();
        int64_t axis = -1;
        auto axis_proto = ctx.getAttribute("axis");
        if (axis_proto) {
          axis = axis_proto->i();
        }
        if (axis < 0) {
          axis += input_ndim;
        }

        if (ctx.getNumOutputs() > 1) {
          auto saved_inv_std_var_shape = ctx.getOutputType(1)->mutable_tensor_type()->mutable_shape();
          saved_inv_std_var_shape->CopyFrom(input_shape);
          saved_inv_std_var_shape->mutable_dim(static_cast<int>(axis))->set_dim_value(1);
        }
      });

  static const char* EfficientNMS_TRT_ver1_doc =
      R"DOC(Efficient NMS TensorRT Plugin.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(EfficientNMS_TRT)
      .SetDomain(kOnnxDomain)
      .SinceVersion(1)
      .SetDoc(EfficientNMS_TRT_ver1_doc)
      .Input(0, "boxes", "The boxes input tensor.", "T")
      .Input(1, "scores", "The scores input tensor.", "T")
      .Input(2, "anchors", "The anchors input tensor.", "T", OpSchema::Optional)
      .Output(0, "num_detections", "The num_detections output tensor.", "tensor(int32)")
      .Output(1, "detection_boxes", "The detection_boxes output tensor.", "T")
      .Output(2, "detection_scores", "The detection_scores output tensor.", "T")
      .Output(3, "detection_classes", "The detection_classes output tensor.", "tensor(int32)")
      .TypeConstraint("T", {"tensor(float)", "tensor(float16)"}, "Constrain input and output types to float tensors.")
      .Attr("background_class", "Background class ID.", AttributeProto::INT)
      .Attr("box_coding", "Encoding type for the boxes or anchors inputs.", AttributeProto::INT)
      .Attr("iou_threshold", "Box IOU threshold value.", AttributeProto::FLOAT)
      .Attr("max_output_boxes", "Max detections to output.", AttributeProto::INT)
      .Attr("plugin_version", "Version number of the TRT plugin.", AttributeProto::STRING)
      .Attr("score_activation", "Activation function to apply to the scores input.", AttributeProto::INT)
      .Attr("score_threshold", "Score threshold value.", AttributeProto::FLOAT)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
          // Type inference
          using namespace ONNX_NAMESPACE;
          ONNX_NAMESPACE::updateOutputElemType(ctx, 0, ONNX_NAMESPACE::TensorProto::INT32);
          propagateElemTypeFromInputToOutput(ctx, 0, 1);
          propagateElemTypeFromInputToOutput(ctx, 0, 2);
          ONNX_NAMESPACE::updateOutputElemType(ctx, 3, ONNX_NAMESPACE::TensorProto::INT32);

          // Shape Inference
          if (!hasInputShape(ctx, 0)) {
            return;
          }
          int64_t max_output_boxes = 1;
          auto max_output_boxes_proto = ctx.getAttribute("max_output_boxes");
          if (max_output_boxes_proto) {
            max_output_boxes = max_output_boxes_proto->i();
          }
          if (max_output_boxes < 1) {
            fail_shape_inference("Attribute 'max_output_boxes' must be >= 1.")
          }

          Dim batch_size;
          unifyInputDim(ctx, 0, 0, batch_size);

          ONNX_NAMESPACE::TensorShapeProto num_detections_shape;
          *num_detections_shape.add_dim() = batch_size;
          num_detections_shape.add_dim()->set_dim_value(1);
          updateOutputShape(ctx, 0, num_detections_shape);

          ONNX_NAMESPACE::TensorShapeProto detection_boxes_shape;
          *detection_boxes_shape.add_dim() = batch_size;
          detection_boxes_shape.add_dim()->set_dim_value(max_output_boxes);
          detection_boxes_shape.add_dim()->set_dim_value(4);
          updateOutputShape(ctx, 1, detection_boxes_shape);

          ONNX_NAMESPACE::TensorShapeProto detection_scores_shape;
          *detection_scores_shape.add_dim() = batch_size;
          detection_scores_shape.add_dim()->set_dim_value(max_output_boxes);
          updateOutputShape(ctx, 2, detection_scores_shape);

          ONNX_NAMESPACE::TensorShapeProto detection_classes_shape;
          *detection_classes_shape.add_dim() = batch_size;
          detection_classes_shape.add_dim()->set_dim_value(max_output_boxes);
          updateOutputShape(ctx, 3, detection_classes_shape);
      });

  static const char* MultilevelCropAndResize_TRT_ver1_doc =
      R"DOC(Multilevel Crop and Resize TensorRT Plugin.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(MultilevelCropAndResize_TRT)
      .SetDomain(kOnnxDomain)
      .SinceVersion(1)
      .SetDoc(MultilevelCropAndResize_TRT_ver1_doc)
      .Input(0, "boxes", "The boxes input tensor.", "T")
      .Input(1, "feature_map_0", "The first feature map input tensor.", "T")
      .Input(2, "feature_map_1", "The second feature map input tensor.", "T")
      .Input(3, "feature_map_2", "The third feature map input tensor.", "T")
      .Input(4, "feature_map_3", "The fourth feature map input tensor.", "T")
      .Output(0, "patches", "The cropped patches output tensor.", "T")
      .TypeConstraint("T", {"tensor(float)"}, "Constrain input and output types to float tensors.")
      .Attr("image_size", "Image size.", AttributeProto::INTS)
      .Attr("pooled_size", "Pooled size.", AttributeProto::INT)
      .Attr("plugin_version", "Version number of the TRT plugin.", AttributeProto::STRING)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
          // Type inference
          propagateElemTypeFromInputToOutput(ctx, 0, 0);

          // Shape Inference
          if (!hasInputShape(ctx, 0)) {
            return;
          }
          int64_t pooled_size = 1;
          auto pooled_size_proto = ctx.getAttribute("pooled_size");
          if (pooled_size_proto) {
            pooled_size = pooled_size_proto->i();
          }
          if (pooled_size < 1) {
            fail_shape_inference("Attribute 'pooled_size' must be >= 1.")
          }

          Dim batch_size, number_boxes, channels;
          unifyInputDim(ctx, 0, 0, batch_size);
          unifyInputDim(ctx, 0, 1, number_boxes);
          unifyInputDim(ctx, 1, 1, channels);

          ONNX_NAMESPACE::TensorShapeProto output_shape;
          *output_shape.add_dim() = batch_size;
          *output_shape.add_dim() = number_boxes;
          *output_shape.add_dim() = channels;
          output_shape.add_dim()->set_dim_value(pooled_size);
          output_shape.add_dim()->set_dim_value(pooled_size);
          updateOutputShape(ctx, 0, output_shape);
      });

  static const char* PyramidROIAlign_TRT_ver1_doc =
      R"DOC(Pyramid ROI Align TensorRT Plugin.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(PyramidROIAlign_TRT)
      .SetDomain(kOnnxDomain)
      .SinceVersion(1)
      .SetDoc(PyramidROIAlign_TRT_ver1_doc)
      .Input(0, "boxes", "The boxes input tensor.", "T")
      .Input(1, "feature_map_0", "The first feature map input tensor.", "T")
      .Input(2, "feature_map_1", "The second feature map input tensor.", "T")
      .Input(3, "feature_map_2", "The third feature map input tensor.", "T")
      .Input(4, "feature_map_3", "The fourth feature map input tensor.", "T")
      .Output(0, "patches", "The cropped patches output tensor.", "T")
      .TypeConstraint("T", {"tensor(float)"}, "Constrain input and output types to float tensors.")
      .Attr("pooled_size", "Pooled size.", AttributeProto::INT)
      .Attr("plugin_version", "Version number of the TRT plugin.", AttributeProto::STRING)
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
          // Type inference
          propagateElemTypeFromInputToOutput(ctx, 0, 0);

          // Shape Inference
          if (!hasInputShape(ctx, 0)) {
            return;
          }
          int64_t pooled_size = 1;
          auto pooled_size_proto = ctx.getAttribute("pooled_size");
          if (pooled_size_proto) {
            pooled_size = pooled_size_proto->i();
          }
          if (pooled_size < 1) {
            fail_shape_inference("Attribute 'pooled_size' must be >= 1.")
          }

          Dim batch_size, number_boxes, channels;
          unifyInputDim(ctx, 0, 0, batch_size);
          unifyInputDim(ctx, 0, 1, number_boxes);
          unifyInputDim(ctx, 1, 1, channels);

          ONNX_NAMESPACE::TensorShapeProto output_shape;
          *output_shape.add_dim() = batch_size;
          *output_shape.add_dim() = number_boxes;
          *output_shape.add_dim() = channels;
          output_shape.add_dim()->set_dim_value(pooled_size);
          output_shape.add_dim()->set_dim_value(pooled_size);
          updateOutputShape(ctx, 0, output_shape);
      });

  static const char* Gelu_ver1_doc =
      R"DOC(Gaussian Error Linear Unit.
A high-performing neural network activation function.The GELU nonlinearity is
the expected transformation of a stochastic regularizer which randomly applies
the identity or zero map to a neuron's input. The GELU nonlinearity weights
inputs by their magnitude, rather than gates inputs by their sign as in ReLUs.)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(Gelu)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(Gelu_ver1_doc)
      .Input(0, "X", "The input data as Tensor.", "T")
      .Output(0, "Y", "The output.", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput)
      .SetContextDependentFunctionBodyBuilder([](const FunctionBodyBuildContext& ctx, const OpSchema& schema, FunctionProto& functionProto) {
        // gelu(x) = x * Phi(x) = x * 1/2(1+erf(x/sqrt(2)))
        auto* tp = ctx.getInputType(0);
        if ((tp == nullptr) || (!tp->has_tensor_type()))
          return false;
        auto elem_type = tp->tensor_type().elem_type();

        FunctionBuilder builder(functionProto);
        builder
            .AddOpset("", 13)
            .Const("Half", 0.5, elem_type)
            .Const("One", 1.0, elem_type)
            .Const("C", std::sqrt(0.5), elem_type)
            .Add(R"(
                CX = Mul (C, X)
                ERFCX = Erf (CX)
                ERFCXPlus1 = Add (ERFCX, One)
                PhiX = Mul (ERFCXPlus1, Half)
                Y = Mul (X, PhiX)
            )");

        schema.BuildFunction(functionProto);
        return true;
      });

  static const char* BiasGelu_ver1_doc =
      R"DOC(Bias Gelu.
It's an extension of Gelu. It takes the sum of input A and bias input B as the input of Gelu activation. )DOC";
  ONNX_CONTRIB_OPERATOR_SCHEMA(BiasGelu)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(BiasGelu_ver1_doc)
      .Input(0, "A", "The normal input data.", "T")
      .Input(1, "B", "The bias input data that is a 1D tensor.", "T")
      .Output(0, "C", "The output.", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  // Used to be ONNX 1.7 Inverse(12)
  // Comment out docs not to increase the binary size
  //
  //  static const char* Inverse_ver1_doc = R"DOC(
  //Calculates inverse of a square matrix or batches of square matrices.
  //Inverse takes one input tensor of shape `[*, M, M]`, where `*` is zero or more batch dimensions,
  //and the inner-most 2 dimensions form square matrices. These matrices must be invertible (full-rank).
  //The behavior where one of the matrices is not invertible is undefined. The implementation can choose
  //to throw an error or output (garbage) results as is. The output is a tensor of shape `[*, M, M]`,
  //containing the individual inverses of all input submatrices.
  //)DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(Inverse)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Input(0, "X", "Input tensor. Every matrix in the batch must be invertible.", "T")
      .Output(0, "Y", "Output tensor of the same type and shape as the input tensor.", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)",
           "tensor(float)",
           "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        // Type inference
        using namespace ONNX_NAMESPACE;
        propagateElemTypeFromInputToOutput(ctx, 0, 0);

        // Shape inference
        if (hasInputShape(ctx, 0)) {
          const TensorShapeProto& input_shape =
              ctx.getInputType(0)->tensor_type().shape();
          const int rank = static_cast<int>(input_shape.dim_size());

          if (rank < 2) {
            fail_shape_inference("Input rank must be >= 2.")
          }

          const auto mat_w = input_shape.dim(rank - 1);
          const auto mat_h = input_shape.dim(rank - 2);
          if (mat_w.has_dim_value() && mat_h.has_dim_value() &&
              (mat_w.dim_value() != mat_h.dim_value())) {
            fail_shape_inference(
                "The inner-most 2 dimensions must have the same size (mat_w:",
                mat_w.dim_value(),
                " != mat_h:",
                mat_h.dim_value(),
                ").");
          }

          // Shape inference
          propagateShapeFromInputToOutput(ctx, 0, 0);
        }
      });

  static const char* TorchEmbedding_ver1_doc = R"DOC(
      Based on Torch operator Embedding, creates a lookup table of embedding vectors of fixed size,
       for a dictionary of fixed size.
      )DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(TorchEmbedding)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(TorchEmbedding_ver1_doc)
      .Input(
          0,
          "weight",
          "The embedding matrix of size N x M. 'N' is equal to the maximum possible index + 1, and 'M' is "
          "equal to the embedding size",
          "T")
      .Input(
          1,
          "indices",
          "Long tensor containing the indices to extract from embedding matrix.",
          "tensor(int64)")
      .Input(
          2,
          "padding_idx",
          "A 0-D scalar tensor. If specified, the entries at `padding_idx` do not contribute to the gradient; "
          "therefore, the embedding vector at `padding_idx` is not updated during training, "
          "i.e. it remains as a fixed pad.",
          "tensor(int64)",
          OpSchema::Optional)
      .Input(
          3,
          "scale_grad_by_freq",
          "A 0-D bool tensor. If given, this will scale gradients by the inverse of frequency of "
          "the indices (words) in the mini-batch. Default  is ``False``",
          "tensor(bool)",
          OpSchema::Optional)
      .Output(
          0,
          "Y",
          "Output tensor of the same type as the input tensor. Shape of the output is * x M, where '*' is the shape of "
          "input indices, and 'M' is the embedding size.",
          "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)",
           "tensor(float)",
           "tensor(double)",
           "tensor(bfloat16)",
           "tensor(uint8)",
           "tensor(uint16)",
           "tensor(uint32)",
           "tensor(uint64)",
           "tensor(int8)",
           "tensor(int16)",
           "tensor(int32)",
           "tensor(int64)"},
          "Constrain input and output types to all numeric tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        using namespace ONNX_NAMESPACE;
        propagateElemTypeFromInputToOutput(ctx, 0, 0);

        TensorShapeProto outputs_shape;
        Dim input_dim_i;

        if (hasInputShape(ctx, 1)) {
          auto& input_shape = getInputShape(ctx, 1);
          for (int32_t i = 0; i < input_shape.dim_size(); i++) {
            input_dim_i = input_shape.dim(i);
            *outputs_shape.add_dim() = input_dim_i;
          }
        }

        Dim embedding_dim;
        unifyInputDim(ctx, 0, 1, embedding_dim);
        *outputs_shape.add_dim() = embedding_dim;
        updateOutputShape(ctx, 0, outputs_shape);
      });

  static const char* Trilu_ver1_doc = R"DOC(
      Returns the upper or lower triangular part of a 2-D matrix, or batches of 2-D matrices. If the attribute "upper" is set to true,
      the upper triangular matrix is retained. Lower triangular matrix is retained otherwise. Default value for upper is true.
      Trilu takes one input tensor of shape [*, N, M], where * is zero or more batch dimensions. The upper triangular part consists
      of the elements on and above the given diagonal (k). The lower triangular part consists of elements on and below the diagonal.
      All other elements in the matrix are set to zero.
      If k = 0, the triangular part on and above/below the main diagonal is retained.
      If upper is set to true, a positive k retains the upper triangular matrix excluding k diagonals above
      the main diagonal. A negative k value includes as many diagonals below the main diagonal.
      If upper is set to false, a positive k retains the lower triangular matrix including k diagonals above
      the main diagonal. A negative k value excludes as many diagonals below the main diagonal.
      )DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(Trilu)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(Trilu_ver1_doc)
      .Attr("upper",
            "Boolean. Indicates whether upper or lower part of matrix is retained. Default is true.",
            AttributeProto::INT,
            static_cast<int64_t>(1))
      .Input(
          0,
          "X",
          "Input tensor of rank 2 or higher.",
          "T")
      .Input(
          1,
          "k",
          "A 0-D tensor containing a single value corresponding to the number diagonals above or the main diagonal to exclude or include."
          "Default value is 0 if it's not specified.",
          "tensor(int64)",
          OpSchema::Optional)
      .Output(
          0,
          "Y",
          "Output tensor of the same type and shape as the input tensor.",
          "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)",
           "tensor(float)",
           "tensor(double)",
           "tensor(bfloat16)",
           "tensor(uint8)",
           "tensor(uint16)",
           "tensor(uint32)",
           "tensor(uint64)",
           "tensor(int8)",
           "tensor(int16)",
           "tensor(int32)",
           "tensor(int64)",
           "tensor(bool)"},
          "Constrain input and output types to all numeric tensors and bool tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        using namespace ONNX_NAMESPACE;
        propagateElemTypeFromInputToOutput(ctx, 0, 0);

        if (hasInputShape(ctx, 0)) {
          const TensorShapeProto& input_shape =
              ctx.getInputType(0)->tensor_type().shape();
          const int rank = static_cast<int>(input_shape.dim_size());
          if (rank < 2) {
            fail_shape_inference("Input rank must be >= 2.")
          }
          propagateShapeFromInputToOutput(ctx, 0, 0);
        }
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(BiasSoftmax)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(
          "Y = softmax(scores + bias)) with simple broadcast on bias. "
          "Intended to specialize softmax(scores + additive_mask) commonly found in transformer models.")
      .Attr("softmax_axis", "apply softmax to elements for dimensions softmax_axis or higher", AttributeProto::INT, static_cast<int64_t>(1))
      .Attr("broadcast_axis", "broadcast bias across input for dimensions broadcast_axis to softmax_axis-1", AttributeProto::INT, static_cast<int64_t>(1))
      .Input(0, "data", "The input data as Tensor.", "T")
      .Input(1, "bias", "The bias (or mask) as Tensor.", "T")
      .Output(0, "output", "The output.", "T")
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain input and output types to float tensors.")
      .TypeAndShapeInferenceFunction(ONNX_NAMESPACE::propagateShapeAndTypeFromFirstInput);

  ONNX_CONTRIB_OPERATOR_SCHEMA(BiasDropout)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(
          "output, dropout_mask = Dropout(data + bias, ratio) + residual, "
          "Intended to specialize the dropout pattern commonly found in transformer models.")
      .Attr("seed", "(Optional) Seed to the random generator, if not specified we will auto generate one.", AttributeProto::INT, OPTIONAL_VALUE)
      .AllowUncheckedAttributes()
      .Input(0, "data", "The input data as Tensor.", "T")
      .Input(1, "bias", "The bias input, a vector with the same shape as last dim of data OR same shape with data", "T")
      .Input(2, "residual", "The residual input, must have the same shape as data", "T", OpSchema::Optional)
      .Input(3, "ratio",
             "The ratio of random dropout, with value in [0, 1). If this input was not set, "
             "or if it was set to 0, the output would be a simple copy of the input. "
             "If it's non-zero, output will be a random dropout of input, which is typically "
             "the case during training.",
             "T1",
             OpSchema::Optional)
      .Input(4, "training_mode",
             "If set to true then it indicates dropout is being used for "
             "training. It is an optional value hence unless specified explicitly, it is false. "
             "If it is false, ratio is ignored and the operation mimics inference mode where nothing "
             "will be dropped from the input data and if mask is requested as output it will contain "
             "all ones.",
             "T2",
             OpSchema::Optional)
      .Output(0, "output", "The output.", "T")
      .Output(1, "mask", "The output mask of dropout.", "T2", OpSchema::Optional)
      .TypeConstraint(
          "T",
          {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
          "Constrain input and output types to float tensors.")
      .TypeConstraint(
          "T1",
          {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
          "Constrain input 'ratio' types to float tensors.")
      .TypeConstraint(
          "T2",
          {"tensor(bool)"},
          "Constrain output 'mask' types to boolean tensors.")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        propagateShapeAndTypeFromFirstInput(ctx);
        if (ctx.getNumOutputs() == 2) {
          updateOutputElemType(ctx, 1, ONNX_NAMESPACE::TensorProto::BOOL);
          if (hasNInputShapes(ctx, 1)) {
            propagateShapeFromInputToOutput(ctx, 0, 1);
          }
        }
      });

  ONNX_CONTRIB_OPERATOR_SCHEMA(IsAllFinite)
      .SetSupportLevel(OpSchema::SupportType::EXPERIMENTAL)
      .SetDoc("IsAllFinite")
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .Attr("isinf_only",
            "If true, check only for Inf, -Inf.",
            AttributeProto::INT,
            static_cast<int64_t>(0))
      .Attr("isnan_only",
            "If true, check only for NaN.",
            AttributeProto::INT,
            static_cast<int64_t>(0))
      .TypeConstraint(
          "V",
          {"tensor(float16)", "tensor(float)", "tensor(double)", "tensor(bfloat16)"},
          "Constrain input and output types to float tensors.")
      .TypeConstraint(
          "T",
          {"tensor(bool)"},
          "Constrain the output to a boolean tensor.")
      .Input(0, "input", "Input tensors to check.", "V",
             OpSchema::Variadic)
      .Output(
          0,
          "output",
          "The output scalar. Its value is true if all input "
          "tensors are finite. Otherwise, the output value would "
          "be false.",
          "T")
      .TypeAndShapeInferenceFunction([](ONNX_NAMESPACE::InferenceContext& ctx) {
        bool isinf_only = static_cast<bool>(getAttribute(ctx, "isinf_only", int64_t(0)));
        bool isnan_only = static_cast<bool>(getAttribute(ctx, "isnan_only", int64_t(0)));
        ORT_ENFORCE(!(isinf_only && isnan_only),
                    "Both attributes isinf_only and isnan_only cannot be set. Unset both to check for both conditions.");
        updateOutputShape(ctx, 0, {});
        updateOutputElemType(ctx, 0, ONNX_NAMESPACE::TensorProto::BOOL);
      });

  static const char* GridSample_ver1_doc = R"DOC(
      Given an `input` and a flow-field `grid`, computes the `output` using `input` values and pixel locations from `grid`.
      Currently, only spatial (4-D) inputs are supported. For `input` with shape (N, C, H, W) and `grid` with shape (N, H_out, W_out, 2),
      the `output` will have shape (N, C, H_out, W_out).
      For each output location `output[n, :, h, w]`, the size-2 vector `grid[n, h, w]` specifies `input` pixel locations `x` and `y`,
      which are used to interpolate the output value `output[n, :, h, w]`.
      The GridSample operator is often used in doing grid generator and sampler in the [Spatial Transformer Networks](https://arxiv.org/abs/1506.02025).
      See also in [torch.nn.functional.grid_sample](https://pytorch.org/docs/master/generated/torch.nn.functional.grid_sample.html#torch-nn-functional-grid-sample).
      )DOC";

  ONNX_CONTRIB_OPERATOR_SCHEMA(GridSample)
      .SetDomain(kMSDomain)
      .SinceVersion(1)
      .SetDoc(GridSample_ver1_doc)
      .Attr(
          "mode",
          "Three interpolation modes: bilinear (default), nearest and bicubic.",
          AttributeProto::STRING,
          std::string("bilinear"))
      .Attr(
          "padding_mode",
          "Support padding modes for outside grid values: `zeros`(default), `border`, `reflection`. "
          "zeros: use 0 for out-of-bound grid locations, "
          "border: use border values for out-of-bound grid locations, "
          "reflection: use values at locations reflected by the border for out-of-bound grid locations.",
          AttributeProto::STRING,
          std::string("zeros"))
      .Attr(
          "align_corners",
          "If align_corners=1, the extrema (-1 and 1) are considered as referring to the center points of the input's corner pixels. "
          "If align_corners=0, they are instead considered as referring to the corner points of the input's corner pixels, making the sampling more resolution agnostic.",
          AttributeProto::INT,
          static_cast<int64_t>(0))
      .Input(
          0,
          "X",
          "4-D tensor of shape (N, C, H, W), "
          "where N is the batch size, C is the numbers of channels, "
          "H and W are the height and width of the input data.",
          "T1")
      .Input(
          1,
          "Grid",
          "Input offset, 4-D tensor of shape (N, H_out, W_out, 2), "
          "where H_out and W_out are the height and width of grid and output, "
          "Grid specifies the sampling pixel locations normalized by the input spatial dimensions. "
          "Therefore, it should have most values in the range of [-1, 1]. "
          "If grid has values outside the range of [-1, 1], the corresponding outputs will be handled as defined by padding_mode.",
          "T1")
      .Output(
          0,
          "Y",
          "4-D tensor of shape (N, C, H_out, W_out).",
          "T2")
      .TypeConstraint(
          "T1",
          OpSchema::all_tensor_types(),
          "Constrain input types to all tensor types.")
      .TypeConstraint(
          "T2",
          {"tensor(float16)", "tensor(float)", "tensor(double)"},
          "Constrain output types to float tensors.")
      .TypeAndShapeInferenceFunction([](InferenceContext& ctx) {
        propagateElemTypeFromInputToOutput(ctx, 0, 0);

        size_t input_param = 0, grid_param = 1;

        checkInputRank(ctx, input_param, 4);
        checkInputRank(ctx, grid_param, 4);

        // Output dimensions, initialized to an unknown-dimension-value
        Dim N, C, H_out, W_out;

        // Get value of N from dim 0 of input_param, if available
        unifyInputDim(ctx, input_param, 0, N);
        // Get value of C from dim 1 of input_param, if available
        unifyInputDim(ctx, input_param, 1, C);

        // Get value of H_out from dim 1 of grid_param, if available
        unifyInputDim(ctx, grid_param, 1, H_out);
        // Get value of W_out from dim 2 of grid_param, if available
        unifyInputDim(ctx, grid_param, 2, W_out);

        // set output shape:
        updateOutputShape(ctx, 0, {N, C, H_out, W_out});
      });

#ifndef _OPSCHEMA_LIB_
  // Register the NCHWc schemas if supported by the platform.
  if (MlasNchwcGetBlockSize() > 1) {
    RegisterNchwcSchemas();
  }
#endif

  RegisterNhwcSchemas();
  RegisterBertSchemas();
  RegisterTextGenerationSchemas();

#ifdef BUILD_MS_EXPERIMENTAL_OPS
  onnxruntime::signal::RegisterSignalSchemas();
#endif

  RegisterQuantizationSchemas();
}
}  // namespace contrib
}  // namespace onnxruntime
