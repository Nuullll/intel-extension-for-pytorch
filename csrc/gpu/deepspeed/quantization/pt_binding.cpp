/*******************************************************************************
 * Copyright 2016-2024 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include <sycl/sycl.hpp>
#include <cassert>
#include <vector>
#include "context.h"
#include "quantization.h"

#include <ATen/ATen.h>
#include <ATen/NestedTensorImpl.h>
#include <ATen/native/nested/NestedTensorUtils.h>

template <typename T>
at::Tensor ds_quantize(at::Tensor& vals, int64_t groups, int64_t bits) {
  auto t_size = vals.sizes();
  int size = 1;
  for (auto dim : t_size)
    size *= dim;

  if ((((size / groups) - 1) / 4096 + 1) <= 256) {
    launch_fake_quantize_kernel(
        (T*)vals.data_ptr(), size, groups, bits, at::getCurrentSYCLStream());
  }
  return vals;
}

template <typename T>
at::Tensor ds_sr_quantize(at::Tensor& vals, int64_t groups, int64_t bits) {
  auto t_size = vals.sizes();
  int size = 1;
  for (auto dim : t_size)
    size *= dim;

  if (((size / groups) / 4 / 1024) <= 256) {
    launch_sr_fake_quantize_kernel(
        (T*)vals.data_ptr(), size, groups, bits, at::getCurrentSYCLStream());
  }
  return vals;
}

template <typename T>
at::Tensor ds_quantize_asym(at::Tensor& vals, int64_t groups, int64_t bits) {
  auto t_size = vals.sizes();
  int size = 1;
  for (auto dim : t_size)
    size *= dim;

  if ((((size / groups) - 1) / 4096 + 1) <= 256) {
    launch_fake_quantize_kernel_asym(
        (T*)vals.data_ptr(), size, groups, bits, at::getCurrentSYCLStream());
  }
  return vals;
}

template <typename T>
at::Tensor ds_sr_quantize_asym(at::Tensor& vals, int64_t groups, int64_t bits) {
  auto t_size = vals.sizes();
  int size = 1;
  for (auto dim : t_size)
    size *= dim;

  if (((size / groups) / 4 / 1024) <= 256) {
    launch_sr_fake_quantize_kernel_asym(
        (T*)vals.data_ptr(), size, groups, bits, at::getCurrentSYCLStream());
  }
  return vals;
}

std::vector<at::Tensor> quantize_kernel(
    at::Tensor& input_vals,
    int64_t groups,
    int64_t numBits,
    bool isSymmetric) {
  quantize::Type quantType =
      isSymmetric ? quantize::Type::Symmetric : quantize::Type::Asymmetric;
  auto dtype = at::kFloat;
  auto params_options = at::TensorOptions()
                            .dtype(dtype)
                            .layout(at::kStrided)
                            .device(at::kXPU)
                            .requires_grad(false);
  const int param_elems = (quantize::requires_offset(quantType)) ? 2 : 1;
  auto params = at::empty({groups, param_elems}, params_options);

  auto output_options = at::TensorOptions()
                            .dtype(at::kChar)
                            .layout(at::kStrided)
                            .device(at::kXPU)
                            .requires_grad(false);

  auto output_sizes = input_vals.sizes().vec();
  output_sizes[output_sizes.size() - 1] /= numBits == 8 ? 1 : 2;
  auto output = at::empty(output_sizes, output_options);

  const int elems_per_group = at::numel(input_vals) / groups;

  launch_quant(
      (int8_t*)output.data_ptr(),
      (float*)params.data_ptr(),
      (sycl::half*)input_vals.data_ptr(),
      (int)groups,
      elems_per_group,
      (int)numBits,
      quantType,
      at::getCurrentSYCLStream());

  return {output, params};
}

template <typename T>
at::Tensor dequantize(
    at::Tensor& quantized_data,
    at::Tensor& params,
    int64_t groups,
    int64_t num_bits,
    bool isSymmetric) {
  quantize::Type quant_type =
      isSymmetric ? quantize::Type::Symmetric : quantize::Type::Asymmetric;
  auto dtype = (std::is_same<T, float>::value) ? at::kFloat : at::kHalf;
  auto output_options = at::TensorOptions()
                            .dtype(dtype)
                            .layout(at::kStrided)
                            .device(at::kXPU)
                            .requires_grad(false);

  auto output_sizes = quantized_data.sizes().vec();
  output_sizes[output_sizes.size() - 1] *= num_bits == 8 ? 1 : 2;
  auto output = at::empty(output_sizes, output_options);

  const int total_elems = at::numel(output);
  const int elems_per_group = total_elems / groups;

  launch_dequantize_kernel(
      (T*)output.data_ptr(),
      (const int8_t*)quantized_data.data_ptr(),
      (const float*)params.data_ptr(),
      quant_type,
      num_bits,
      elems_per_group,
      total_elems,
      at::getCurrentSYCLStream());

  return output;
}

at::Tensor dequantize_int4_to_half_experimental(
    at::Tensor& data_in,
    at::Tensor& scale_buffer,
    at::Tensor& min_val_buffer,
    int64_t num_group,
    int64_t group_size) {
  auto output_options = at::TensorOptions().dtype(at::kHalf).device(at::kXPU);
  auto output = at::empty({num_group, group_size}, output_options);

  launch_dequantize_int4_to_half_experimental(
      (uint8_t*)data_in.data_ptr(),
      (sycl::half*)output.data_ptr(),
      (sycl::half*)scale_buffer.data_ptr(),
      (sycl::half*)min_val_buffer.data_ptr(),
      num_group,
      group_size,
      at::getCurrentSYCLStream());

  return output;
}

at::Tensor dequantize_int8_to_half_experimental(
    at::Tensor& data_in,
    at::Tensor& scale_buffer,
    at::Tensor& min_val_buffer,
    int64_t num_group,
    int64_t group_size) {
  auto output_options = at::TensorOptions().dtype(at::kHalf).device(at::kXPU);
  auto output = at::empty({num_group, group_size}, output_options);

  launch_dequantize_int8_to_half_experimental(
      (uint8_t*)data_in.data_ptr(),
      (sycl::half*)output.data_ptr(),
      (sycl::half*)scale_buffer.data_ptr(),
      (sycl::half*)min_val_buffer.data_ptr(),
      num_group,
      group_size,
      at::getCurrentSYCLStream());

  return output;
}

std::vector<at::Tensor> ds_swizzle_quant(
    at::Tensor& input_vals,
    int64_t groups,
    int64_t num_bits,
    bool isSymmetric,
    int64_t pipeline_size,
    int64_t nodes,
    int64_t devices_per_node) {
  quantize::Type quant_type =
      isSymmetric ? quantize::Type::Symmetric : quantize::Type::Asymmetric;
  auto scales_options = at::TensorOptions()
                            .dtype(at::kFloat)
                            .layout(at::kStrided)
                            .device(at::kXPU)
                            .requires_grad(false);
  const int scales_elems = (quantize::requires_offset(quant_type)) ? 2 : 1;
  auto scales = at::empty({groups, scales_elems}, scales_options);

  auto output_options = at::TensorOptions()
                            .dtype(at::kChar)
                            .layout(at::kStrided)
                            .device(at::kXPU)
                            .requires_grad(false);

  const int quantization_scalar = 8 / num_bits;
  const int compressed_vals = at::numel(input_vals) / quantization_scalar;

  auto output = at::empty({compressed_vals}, output_options);
  const int elems_per_group = at::numel(input_vals) / groups;

  launch_swizzled_quant(
      (int8_t*)output.data_ptr(),
      (float*)scales.data_ptr(),
      (sycl::half*)input_vals.data_ptr(),
      num_bits,
      quant_type,
      groups,
      elems_per_group,
      pipeline_size,
      nodes,
      devices_per_node,
      at::getCurrentSYCLStream());

  return {output, scales};
}

std::vector<at::Tensor> quantized_reduction(
    at::Tensor& input_vals,
    at::Tensor& input_scales,
    int64_t in_groups,
    int64_t out_groups,
    int64_t num_bits,
    bool isSymmetric,
    int64_t devices_per_node) {
  quantize::Type quant_type =
      isSymmetric ? quantize::Type::Symmetric : quantize::Type::Asymmetric;
  auto scales_options = at::TensorOptions()
                            .dtype(at::kFloat)
                            .layout(at::kStrided)
                            .device(at::kXPU)
                            .requires_grad(false);
  const int scales_elems = (quantize::requires_offset(quant_type)) ? 2 : 1;
  auto scales = at::empty({out_groups, scales_elems}, scales_options);

  auto output_options = at::TensorOptions()
                            .dtype(at::kChar)
                            .layout(at::kStrided)
                            .device(at::kXPU)
                            .requires_grad(false);

  std::vector<long int> sz(
      input_vals.sizes().begin(), input_vals.sizes().end());
  sz[sz.size() - 1] = sz.back() / devices_per_node; // num of GPU per nodes
  const int elems_per_in_tensor = at::numel(input_vals) / devices_per_node;
  auto output = at::empty(input_vals.sizes(), output_options);

  const int elems_per_in_group =
      elems_per_in_tensor / (in_groups / devices_per_node);
  const int elems_per_out_group = elems_per_in_tensor / out_groups;

  launch_dequant_reduce(
      (int8_t*)output.data_ptr(),
      (float*)scales.data_ptr(),
      (const int8_t*)input_vals.data_ptr(),
      (const float*)input_scales.data_ptr(),
      devices_per_node,
      num_bits,
      quant_type,
      out_groups,
      elems_per_out_group,
      elems_per_in_tensor,
      in_groups / devices_per_node,
      elems_per_in_group,
      at::getCurrentSYCLStream());
  return {output, scales};
}

DS_LIBRARY_FRAGMENT() {
  DS_OP_REGISTER(
      "ds_quantize_fp32", ds_quantize<float>, c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "ds_quantize_fp16",
      ds_quantize<sycl::half>,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "ds_sr_quantize_fp32",
      ds_sr_quantize<float>,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "ds_sr_quantize_fp16",
      ds_sr_quantize<sycl::half>,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "ds_quantize_asym_fp32",
      ds_quantize_asym<float>,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "ds_quantize_asym_fp16",
      ds_quantize_asym<sycl::half>,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "ds_sr_quantize_asym_fp32",
      ds_sr_quantize_asym<float>,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "ds_sr_quantize_asym_fp16",
      ds_sr_quantize_asym<sycl::half>,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER("quantize", quantize_kernel, c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "dequantize", dequantize<sycl::half>, c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "dequantize_fp32", dequantize<float>, c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "dequantize_int4_to_half_experimental",
      dequantize_int4_to_half_experimental,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "dequantize_int8_to_half_experimental",
      dequantize_int8_to_half_experimental,
      c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "swizzle_quant", ds_swizzle_quant, c10::DispatchKey::AutogradXPU);
  DS_OP_REGISTER(
      "quantized_reduction",
      quantized_reduction,
      c10::DispatchKey::AutogradXPU);
}
