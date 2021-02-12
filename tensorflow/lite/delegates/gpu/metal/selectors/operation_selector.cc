/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/delegates/gpu/metal/selectors/operation_selector.h"

#include <vector>

#include "absl/strings/substitute.h"
#include "tensorflow/lite/delegates/gpu/common/gpu_info.h"
#include "tensorflow/lite/delegates/gpu/common/model.h"
#include "tensorflow/lite/delegates/gpu/common/model_hints.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/selectors/convolution_transposed_selector.h"
#include "tensorflow/lite/delegates/gpu/common/selectors/default_selector.h"
#include "tensorflow/lite/delegates/gpu/common/selectors/dw_convolution_selector.h"
#include "tensorflow/lite/delegates/gpu/common/selectors/fully_connected_selector.h"
#include "tensorflow/lite/delegates/gpu/common/selectors/simple_selectors.h"
#include "tensorflow/lite/delegates/gpu/common/selectors/subgraph.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/task/tensor_desc.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/conv_metal.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/elementwise.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/mean_stddev_normalization.h"
#include "tensorflow/lite/delegates/gpu/common/util.h"
#include "tensorflow/lite/delegates/gpu/common/winograd_util.h"

namespace tflite {
namespace gpu {
namespace metal {
namespace {
bool IsRecommendedForWinograd4x4To6x6(const Convolution2DAttributes& attr,
                                      const GpuInfo& gpu_info,
                                      const BHWC& dst_shape) {
  const int tiles_x = DivideRoundUp(dst_shape.w, 4);
  const int tiles_y = DivideRoundUp(dst_shape.h, 4);
  const int total_tiles = tiles_x * tiles_y;
  const int src_depth = DivideRoundUp(attr.weights.shape.i, 4);
  const int dst_depth = DivideRoundUp(attr.weights.shape.o, 4);
  int min_depth = 16;
  const int min_tiles = 32;
  if (total_tiles >= min_tiles * 8) {
    min_depth /= 4;
    min_depth = std::max(min_depth, 8);
  } else if (total_tiles >= min_tiles * 4) {
    min_depth /= 2;
    min_depth = std::max(min_depth, 8);
  }
  const bool recommended_channels =
      src_depth >= min_depth && dst_depth >= min_depth;
  const bool recommended_hw = total_tiles >= min_tiles;
  return recommended_channels && recommended_hw;
}

absl::Status WinogradFromNode(const GpuInfo& gpu_info,
                              const std::vector<Value*>& inputs,
                              const std::vector<Value*>& outputs,
                              const OperationDef& op_def,
                              const BHWC& input_shape, const BHWC& output_shape,
                              const Convolution2DAttributes& attr,
                              GPUOperationsSubgraph* gpu_subgraph) {
  if (!IsSuitableForWinograd4x4To6x6(attr)) {
    return absl::UnimplementedError("No implementation for this case.");
  }
  if (!IsRecommendedForWinograd4x4To6x6(attr, gpu_info, output_shape)) {
    return absl::UnimplementedError("Not recommended for this case.");
  }

  const int tiles_x = DivideRoundUp(output_shape.w, 4);
  const int tiles_y = DivideRoundUp(output_shape.h, 4);
  const BHWC shape_0{input_shape.b, 36, tiles_x * tiles_y, input_shape.c};
  const BHWC shape_1{input_shape.b, 36, tiles_x * tiles_y, output_shape.c};
  TensorDescriptor tensor_desc = op_def.src_tensors[0];
  gpu_subgraph->new_tensors = {{shape_0, tensor_desc}, {shape_1, tensor_desc}};
  gpu_subgraph->operations.clear();
  gpu_subgraph->operations.resize(3);

  OperationDef winograd_up_def;
  winograd_up_def.precision = op_def.precision;
  winograd_up_def.src_tensors.push_back(op_def.src_tensors[0]);
  winograd_up_def.dst_tensors.push_back(op_def.src_tensors[0]);
  auto& winograd_up = gpu_subgraph->operations[0];
  winograd_up.operation =
      SelectWinograd4x4To36(gpu_info, attr.padding, winograd_up_def);
  winograd_up.input_ids = {static_cast<int>(inputs[0]->id)};
  winograd_up.output_ids = {-1};

  OperationDef conv_def;
  conv_def.precision = op_def.precision;
  conv_def.src_tensors.push_back(op_def.src_tensors[0]);
  conv_def.dst_tensors.push_back(op_def.src_tensors[0]);
  auto& conv = gpu_subgraph->operations[1];
  conv.input_ids = {-1};
  conv.output_ids = {-2};
  auto gpu_op =
      CreateConvolutionMetalWino4x4To6x6(conv_def, shape_1, attr, gpu_info);
  conv.operation = absl::make_unique<ConvolutionMetal>(std::move(gpu_op));
  OperationDef winograd_down_def;
  winograd_down_def.precision = op_def.precision;
  winograd_down_def.src_tensors.push_back(op_def.src_tensors[0]);
  winograd_down_def.dst_tensors.push_back(op_def.dst_tensors[0]);
  auto& winograd_down = gpu_subgraph->operations[2];
  winograd_down.input_ids = {-2};
  winograd_down.output_ids = {static_cast<int>(outputs[0]->id)};
  winograd_down.operation =
      SelectWinograd36To4x4(gpu_info, winograd_down_def, attr.bias);
  return absl::OkStatus();
}

}  // namespace

absl::Status GPUOperationFromNode(const GpuInfo& gpu_info,
                                  const OperationDef& op_def,
                                  const std::vector<Value*>& inputs,
                                  const std::vector<Value*>& outputs,
                                  const Node& node,
                                  GPUOperationsSubgraph* gpu_subgraph) {
  std::unique_ptr<GPUOperation>* gpu_op =
      InitSingleOpSubgraph(inputs, outputs, gpu_subgraph);
  auto op_type = OperationTypeFromString(node.operation.type);
  switch (op_type) {
    case OperationType::ADD: {
      if (inputs.size() == 2 &&
          (inputs[0]->tensor.shape.c == inputs[1]->tensor.shape.c ||
           inputs[1]->tensor.shape.c == 1)) {
        GPUOperation operation =
            CreateElementwiseTwoInput(op_def, op_type, inputs[1]->tensor.shape);
        *gpu_op = absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      } else if (inputs.size() >= 2) {
        auto output = outputs[0];
        std::vector<int> channels(inputs.size());
        for (int i = 0; i < inputs.size(); ++i) {
          channels[i] = inputs[i]->tensor.shape.c;
        }
        SelectAdd(op_def, channels, output->tensor.shape.c, gpu_op);
        return absl::OkStatus();
      } else if (inputs.size() == 1 && node.operation.attributes.has_value()) {
        auto attr =
            absl::any_cast<ElementwiseAttributes>(node.operation.attributes);
        GPUOperation operation =
            CreateElementwise(gpu_info, op_def, op_type, attr);
        *gpu_op = absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      }
      return absl::UnimplementedError(absl::StrCat(
          "No support of ", node.operation.type, " with this parameters"));
    }
    case OperationType::CONCAT: {
      auto attr = absl::any_cast<ConcatAttributes>(node.operation.attributes);
      std::vector<int> channels(inputs.size());
      for (int i = 0; i < inputs.size(); ++i) {
        channels[i] = inputs[i]->tensor.shape.c;
      }
      return SelectConcat(attr, channels, op_def, gpu_info, gpu_op);
    }
    case OperationType::CONVOLUTION_2D: {
      if (inputs.size() != 1) {
        return absl::UnimplementedError(
            "Convolution does not support more than 1 runtime tensor");
      }
      auto attr =
          absl::any_cast<Convolution2DAttributes>(node.operation.attributes);
      auto input_shape = inputs[0]->tensor.shape;
      auto output_shape = outputs[0]->tensor.shape;
      if (WinogradFromNode(gpu_info, inputs, outputs, op_def, input_shape,
                           output_shape, attr, gpu_subgraph)
              .ok()) {
        return absl::OkStatus();
      } else {
        auto conv_op =
            CreateConvolutionMetal(op_def, output_shape, attr, gpu_info);
        *gpu_op = absl::make_unique<ConvolutionMetal>(std::move(conv_op));
      }
      break;
    }
    case OperationType::CONVOLUTION_TRANSPOSED: {
      if (inputs.size() != 1) {
        return absl::UnimplementedError(
            "Convolution Transposed does not support more than 1 runtime "
            "tensor");
      }
      auto attr = absl::any_cast<ConvolutionTransposedAttributes>(
          node.operation.attributes);
      *gpu_op = SelectConvolutionTransposed(attr, gpu_info, op_def);
      return absl::OkStatus();
    }
    case OperationType::DEPTHWISE_CONVOLUTION: {
      auto attr = absl::any_cast<DepthwiseConvolution2DAttributes>(
          node.operation.attributes);
      if (inputs.size() == 1) {
        *gpu_op = SelectDWConvolution(attr, gpu_info, op_def);
      } else {
        if (inputs[1]->tensor.shape.b != 1) {
          return absl::UnimplementedError(
              "No support of depthwise runtime weights with channel multiplier "
              "!= 1");
        }
        *gpu_op = SelectDWConvolutionDynamicWeights(attr, gpu_info, op_def);
      }
      return absl::OkStatus();
    }
    case OperationType::FULLY_CONNECTED: {
      auto attr =
          absl::any_cast<FullyConnectedAttributes>(node.operation.attributes);
      *gpu_op = SelectFullyConnected(attr, gpu_info, op_def,
                                     inputs[0]->tensor.shape.b);
      return absl::OkStatus();
    }
    case OperationType::LSTM: {
      *gpu_op = SelectLSTM(op_def, gpu_info);
      return absl::OkStatus();
    }
    case OperationType::MAX_UNPOOLING_2D: {
      auto attr =
          absl::any_cast<MaxUnpooling2DAttributes>(node.operation.attributes);
      *gpu_op = SelectMaxUnpooling(attr, op_def);
      return absl::OkStatus();
    }
    case OperationType::MEAN: {
      auto attr = absl::any_cast<MeanAttributes>(node.operation.attributes);
      *gpu_op = SelectReduce(attr.dims, inputs[0]->tensor.shape, op_type,
                             op_def, gpu_info);
      return absl::OkStatus();
    }
    case OperationType::MEAN_STDDEV_NORMALIZATION: {
      MeanStdDevNormalization operation = CreateMeanStdDevNormalization(
          op_def, gpu_info, (inputs[0]->tensor.shape.c + 3) / 4);
      *gpu_op =
          absl::make_unique<MeanStdDevNormalization>(std::move(operation));
      return absl::OkStatus();
    }
    case OperationType::PAD: {
      auto attr = absl::any_cast<PadAttributes>(node.operation.attributes);
      SelectPadding(attr, op_def, gpu_op);
      return absl::OkStatus();
    }
    case OperationType::POOLING_2D: {
      auto attr =
          absl::any_cast<Pooling2DAttributes>(node.operation.attributes);
      *gpu_op = SelectPooling(attr, op_def);
      return absl::OkStatus();
    }
    case OperationType::PRELU: {
      auto attr = absl::any_cast<PReLUAttributes>(node.operation.attributes);
      *gpu_op = SelectPReLU(attr, gpu_info, op_def);
      return absl::OkStatus();
    }
    case OperationType::REDUCE_MAXIMUM:
    case OperationType::REDUCE_MINIMUM:
    case OperationType::REDUCE_PRODUCT:
    case OperationType::REDUCE_SUM: {
      auto attr = absl::any_cast<ReduceAttributes>(node.operation.attributes);
      *gpu_op = SelectReduce(attr.dims, inputs[0]->tensor.shape, op_type,
                             op_def, gpu_info);
      return absl::OkStatus();
    }
    case OperationType::RELU: {
      auto attr = absl::any_cast<ReLUAttributes>(node.operation.attributes);
      *gpu_op = SelectReLU(attr, op_def);
      return absl::OkStatus();
    }
    case OperationType::QUANTIZE_AND_DEQUANTIZE: {
      auto attr = absl::any_cast<QuantizeAndDequantizeAttributes>(
          node.operation.attributes);
      *gpu_op = SelectQuantizeAndDequantize(attr, op_def);
      return absl::OkStatus();
    }
    case OperationType::RESHAPE: {
      const int src_channels = inputs[0]->tensor.shape.c;
      auto attr = absl::any_cast<ReshapeAttributes>(node.operation.attributes);
      SelectReshape(src_channels, attr.new_shape.c, op_def, gpu_op);
      return absl::OkStatus();
    }
    case OperationType::RESIZE: {
      auto attr = absl::any_cast<Resize2DAttributes>(node.operation.attributes);
      return SelectResize(attr, op_def, gpu_op);
    }
    case OperationType::SLICE: {
      auto attr = absl::any_cast<SliceAttributes>(node.operation.attributes);
      SelectStridedSlice(attr, op_def, gpu_op);
      return absl::OkStatus();
    }
    case OperationType::SOFTMAX: {
      SelectSoftmax(inputs[0]->tensor.shape, op_def, gpu_op);
      return absl::OkStatus();
    }
    case OperationType::SPACE_TO_DEPTH: {
      auto attr =
          absl::any_cast<SpaceToDepthAttributes>(node.operation.attributes);
      SelectSpaceToDepth(attr, op_def, gpu_op);
      return absl::OkStatus();
    }
    case OperationType::TRANSPOSE: {
      auto attr =
          absl::any_cast<TransposeAttributes>(node.operation.attributes);
      SelectTranspose(attr, op_def, gpu_op);
      return absl::OkStatus();
    }
    case OperationType::ABS:
    case OperationType::COPY:
    case OperationType::COS:
    case OperationType::ELU:
    case OperationType::EXP:
    case OperationType::HARD_SWISH:
    case OperationType::LOG:
    case OperationType::NEG:
    case OperationType::RSQRT:
    case OperationType::SIGMOID:
    case OperationType::SIN:
    case OperationType::SQRT:
    case OperationType::SQUARE:
    case OperationType::TANH: {
      GPUOperation operation =
          CreateElementwiseOneInput(gpu_info, op_def, op_type);
      *gpu_op = absl::make_unique<GPUOperation>(std::move(operation));
      return absl::OkStatus();
    }
    case OperationType::DIV:
    case OperationType::EQUAL:
    case OperationType::GREATER:
    case OperationType::GREATER_EQUAL:
    case OperationType::LESS:
    case OperationType::LESS_EQUAL:
    case OperationType::MAXIMUM:
    case OperationType::MINIMUM:
    case OperationType::MUL:
    case OperationType::NOT_EQUAL:
    case OperationType::POW:
    case OperationType::SQUARED_DIFF:
    case OperationType::SUB: {
      if (inputs.size() == 2) {
        GPUOperation operation =
            CreateElementwiseTwoInput(op_def, op_type, inputs[1]->tensor.shape);
        *gpu_op = absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      } else if (inputs.size() == 1 && node.operation.attributes.has_value()) {
        auto attr =
            absl::any_cast<ElementwiseAttributes>(node.operation.attributes);
        GPUOperation operation =
            CreateElementwise(gpu_info, op_def, op_type, attr);
        *gpu_op = absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      }
      return absl::UnimplementedError(absl::StrCat(
          "No support of ", node.operation.type, " with this parameters"));
    }
    case OperationType::BATCH_NORMALIZATION:
    case OperationType::BATCH_TO_SPACE:
    case OperationType::BATCHED_MATMUL:
    case OperationType::CONSTANT:
    case OperationType::SPACE_TO_BATCH:
      return absl::UnimplementedError("Unsupported op: " + node.operation.type);
    default: {
      ModelHints hints;
      return SelectDefault(gpu_info, op_def, hints, inputs, outputs, node,
                           gpu_subgraph);
    }
  }
  return absl::OkStatus();
}

}  // namespace metal
}  // namespace gpu
}  // namespace tflite
