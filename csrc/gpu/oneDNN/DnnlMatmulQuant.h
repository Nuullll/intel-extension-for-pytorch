#pragma once

#include <ATen/ATen.h>
#include <ATen/record_function.h>

#include <oneDNN/Runtime.h>
#include <runtime/Utils.h>
#include <tensor/Tensor.h>
#include <utils/LRUCache.h>
#include "DnnlExt.h"
#include "Utils.h"

#include <oneapi/dnnl/dnnl.hpp>
#include <cstdint>

using namespace dnnl;
using namespace torch_ipex::xpu::oneDNN;

#ifdef USE_PRIMITIVE_CACHE

namespace torch_ipex::xpu {
namespace oneDNN {

inline Tensor resize_as_onednn_mat1(const Tensor& mat1, const Tensor& output) {
  auto output_ = output.flatten(0, -2);
  int n = output_.sizes()[1];
  auto sizes = mat1.sym_sizes().vec();
  sizes[sizes.size() - 1] = n;
  return output.view_symint(sizes);
}

static at::Tensor dnnl_matmul_w4a16(
    Tensor& result, // dst, [b, m, n]
    const Tensor& mat1_, // src, [b, m, k]
    const Tensor& mat2, // quantized weight, [k, n] transpose
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [k/group_size, n]
    const Tensor& zp, // [k/group_size, n/8]
    int64_t group_size,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION("dnnl_matmul_w4a16", std::vector<c10::IValue>({mat1_, mat2}));
  auto mat1 = g_idx.has_value() ? mat1_.index_select(-1, g_idx.value()) : mat1_;
  auto o_sz = mat1.sizes().vec();
  auto b_sz = mat2.sizes();
  *(o_sz.end() - 1) = *(b_sz.end() - 1);
  result = at::empty(o_sz, mat1.options());

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);

  // validate bias and make it compatible with oneDNN implementation
  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1,
      mat2,
      bias,
      result,
      scale,
      zp,
      group_size,
      engine,
      [](primitive_attr& pattr) {});

  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
        });
  }

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;
  arg_handles.reserve(8);
  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias.has_value()) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();

  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);
  return result;
}

static at::Tensor dnnl_matmul_w4a16_and_silu(
    Tensor& result, // dst, [b, M, N]
    const Tensor& mat1_, // src, [b, M, K]
    const Tensor& mat2_, // quantized weight, [K/8, N]
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [K/group_size, N]
    const Tensor& zp, // [k/group_size, N/8]
    int64_t group_size,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION(
      "dnnl_matmul_w4a16_and_silu", std::vector<c10::IValue>({mat1_, mat2_}));
  Tensor mat1;
  if (g_idx.has_value()) {
    mat1 = mat1_.index_select(-1, g_idx.value()).flatten(0, -2);
  } else {
    mat1 = mat1_.flatten(0, -2);
  }
  auto mat2 = mat2_.flatten(0, -2);
  int m = mat1.sizes()[0];
  int n = mat2.sizes()[1];
  int k = mat1.sizes()[1];
  result = at::empty({m, n}, mat1_.options());
  size_t dims = result.dim();

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device

  auto silu = [&](primitive_attr& pattr) {
    post_ops po;
    po.append_eltwise(algorithm::eltwise_swish, 1.f, 0.f);
    pattr.set_post_ops(po);
  };

  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1, mat2, bias, result, scale, zp, group_size, engine, silu);
  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
          /* return dpcpp_onednn_memory(get_onednn_md(zp), engine,
           * zp.data_ptr());
           */
        });
  }

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;

  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias.has_value()) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();

  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);

  result = resize_as_onednn_mat1(mat1_, result);
  return result;
}

static at::Tensor dnnl_matmul_w4a16_and_resmul(
    Tensor& result, // dst, [b, M, N]
    const Tensor& mat1_, // src, [b, M, K]
    const Tensor& mat2_, // quantized weight, [K/8, N]
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [K/group_size, N]
    const Tensor& zp, // [k/group_size, N/8]
    const Tensor& res,
    int64_t group_size,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION(
      "dnnl_matmul_w4a16_and_resmul", std::vector<c10::IValue>({mat1_, mat2_}));
  Tensor mat1;
  if (g_idx.has_value()) {
    mat1 = mat1_.index_select(-1, g_idx.value()).flatten(0, -2);
  } else {
    mat1 = mat1_.flatten(0, -2);
  }
  auto mat2 = mat2_.flatten(0, -2);
  int m = mat1.sizes()[0];
  int n = mat2.sizes()[1];
  int k = mat1.sizes()[1];
  result = at::empty({m, n}, mat1_.options());
  size_t dims = result.dim();

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device

  auto res_flat = res.flatten(0, -2);
  auto resmul = [&](primitive_attr& pattr) {
    post_ops po;
    po.append_binary(algorithm::binary_mul, get_onednn_md(res_flat));
    pattr.set_post_ops(po);
  };

  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1, mat2, bias, result, scale, zp, group_size, engine, resmul);
  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
          /* return dpcpp_onednn_memory(get_onednn_md(zp), engine,
           * zp.data_ptr());
           */
        });
  }

  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_MULTIPLE_POST_OP(0) | DNNL_ARG_SRC_1,
      res_flat.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(res_flat), engine, res_flat.data_ptr());
      });

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;

  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias.has_value()) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();
  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);

  result = resize_as_onednn_mat1(mat1_, result);
  return result;
}

static at::Tensor dnnl_matmul_w4a16_and_bias_gelu(
    Tensor& result, // dst, [b, M, N]
    const Tensor& mat1_, // src, [b, M, K]
    const Tensor& mat2_, // quantized weight, [K/8, N]
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [K/group_size, N]
    const Tensor& zp, // [k/group_size, N/8]
    int64_t group_size,
    c10::string_view approximate,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION(
      "dnnl_matmul_w4a16_and_bias_gelu",
      std::vector<c10::IValue>({mat1_, mat2_}));
  Tensor mat1;
  if (g_idx.has_value()) {
    mat1 = mat1_.index_select(-1, g_idx.value()).flatten(0, -2);
  } else {
    mat1 = mat1_.flatten(0, -2);
  }
  auto mat2 = mat2_.flatten(0, -2);
  int m = mat1.sizes()[0];
  int n = mat2.sizes()[1];
  int k = mat1.sizes()[1];
  result = at::empty({m, n}, mat1_.options());
  size_t dims = result.dim();

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device

  auto bias_gelu = [&](primitive_attr& pattr) {
    post_ops po;
    if (approximate == "none") {
      po.append_eltwise(algorithm::eltwise_gelu_erf, 1.f, 0.f);
    } else if (approximate == "tanh") {
      po.append_eltwise(algorithm::eltwise_gelu_tanh, 1.f, 0.f);
    } else {
      TORCH_INTERNAL_ASSERT(false, "Unsupported gelu algorithm: ", approximate);
    }
    pattr.set_post_ops(po);
  };

  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1, mat2, bias, result, scale, zp, group_size, engine, bias_gelu);
  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
          /* return dpcpp_onednn_memory(get_onednn_md(zp), engine,
           * zp.data_ptr());
           */
        });
  }

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;

  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias.has_value()) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();

  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);
  result = resize_as_onednn_mat1(mat1_, result);
  return result;
}

static at::Tensor dnnl_matmul_w4a16_and_bias_resadd_resadd(
    Tensor& result, // dst, [b, M, N]
    const Tensor& mat1_, // src, [b, M, K]
    const Tensor& mat2, // quantized weight, [K/8, N]
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [K/group_size, N]
    const Tensor& zp, // [k/group_size, N/8]
    const Tensor& res,
    const Tensor& res1,
    int64_t group_size,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION(
      "dnnl_matmul_w4a16_and_bias_resadd_resadd",
      std::vector<c10::IValue>({mat1_, mat2}));

  auto mat1 = g_idx.has_value() ? mat1_.index_select(-1, g_idx.value()) : mat1_;
  auto o_sz = mat1.sizes().vec();
  auto b_sz = mat2.sizes();
  *(o_sz.end() - 1) = *(b_sz.end() - 1);
  result = at::empty(o_sz, mat1.options());

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device

  auto bias_resadd_resadd = [&](primitive_attr& pattr) {
    auto res_flat = res.flatten(0, -2);
    auto res1_flat = res1.flatten(0, -2);
    post_ops po;
    po.append_binary(algorithm::binary_add, get_onednn_md(res_flat));
    po.append_binary(algorithm::binary_add, get_onednn_md(res1_flat));
    pattr.set_post_ops(po);
  };

  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1,
      mat2,
      bias,
      result,
      scale,
      zp,
      group_size,
      engine,
      bias_resadd_resadd);

  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
          /* return dpcpp_onednn_memory(get_onednn_md(zp), engine,
           * zp.data_ptr());
           */
        });
  }

  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_MULTIPLE_POST_OP(0) | DNNL_ARG_SRC_1,
      res.data_ptr(),
      [&]() {
        auto res_flat = res.flatten(0, -2);
        return dpcpp_onednn_memory(
            get_onednn_md(res_flat), engine, res_flat.data_ptr());
      });
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_MULTIPLE_POST_OP(1) | DNNL_ARG_SRC_1,
      res1.data_ptr(),
      [&]() {
        auto res1_flat = res1.flatten(0, -2);
        return dpcpp_onednn_memory(
            get_onednn_md(res1_flat), engine, res1_flat.data_ptr());
      });

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;
  arg_handles.reserve(8);

  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias.has_value()) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();
  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);
  return result;
}

static at::Tensor dnnl_matmul_w4a16_and_silu_mul(
    Tensor& result, // dst, [b, M, N]
    const Tensor& mat1_, // src, [b, M, K]
    const Tensor& mat2_, // quantized weight, [K/8, N]
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [K/group_size, N]
    const Tensor& zp, // [k/group_size, N/8]
    const Tensor& res,
    int64_t group_size,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION(
      "dnnl_matmul_w4a16_and_silu_mul",
      std::vector<c10::IValue>({mat1_, mat2_}));
  Tensor mat1;
  if (g_idx.has_value()) {
    mat1 = mat1_.index_select(-1, g_idx.value()).flatten(0, -2);
  } else {
    mat1 = mat1_.flatten(0, -2);
  }
  auto mat2 = mat2_.flatten(0, -2);
  int m = mat1.sizes()[0];
  int n = mat2.sizes()[1];
  int k = mat1.sizes()[1];
  result = at::empty({m, n}, mat1_.options());
  size_t dims = result.dim();

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device

  auto res_flat = res.flatten(0, -2);
  auto silu_mul = [&](primitive_attr& pattr) {
    post_ops po;
    po.append_eltwise(algorithm::eltwise_swish, 1.f, 0.f);
    po.append_binary(algorithm::binary_mul, get_onednn_md(res_flat));
    pattr.set_post_ops(po);
  };

  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1, mat2, bias, result, scale, zp, group_size, engine, silu_mul);
  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
          /* return dpcpp_onednn_memory(get_onednn_md(zp), engine,
           * zp.data_ptr());
           */
        });
  }

  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_MULTIPLE_POST_OP(1) | DNNL_ARG_SRC_1,
      res_flat.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(res_flat), engine, res_flat.data_ptr());
      });

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;

  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias.has_value()) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();

  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);
  result = resize_as_onednn_mat1(mat1_, result);
  return result;
}

static at::Tensor dnnl_matmul_w4a16_and_bias_silu_mul(
    Tensor& result, // dst, [b, M, N]
    const Tensor& mat1_, // src, [b, M, K]
    const Tensor& mat2, // quantized weight, [K/8, N]
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [K/group_size, N]
    const Tensor& zp, // [k/group_size, N/8]
    const Tensor& res,
    int64_t group_size,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION(
      "dnnl_matmul_w4a16_and_bias_silu_mul",
      std::vector<c10::IValue>({mat1_, mat2}));

  auto mat1 = g_idx.has_value() ? mat1_.index_select(-1, g_idx.value()) : mat1_;
  auto o_sz = mat1.sizes().vec();
  auto b_sz = mat2.sizes();
  *(o_sz.end() - 1) = *(b_sz.end() - 1);
  result = at::empty(o_sz, mat1.options());

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device

  auto silu_mul_int4 = [&](primitive_attr& pattr) {
    auto res_flat = res.flatten(0, -2);
    post_ops po;
    po.append_eltwise(algorithm::eltwise_swish, 1.f, 0.f);
    po.append_binary(algorithm::binary_mul, get_onednn_md(res_flat));
    pattr.set_post_ops(po);
  };

  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1, mat2, bias, result, scale, zp, group_size, engine, silu_mul_int4);

  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
          /* return dpcpp_onednn_memory(get_onednn_md(zp), engine,
           * zp.data_ptr());
           */
        });
  }

  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_MULTIPLE_POST_OP(1) | DNNL_ARG_SRC_1,
      res.data_ptr(),
      [&]() {
        auto res_flat = res.flatten(0, -2);
        return dpcpp_onednn_memory(
            get_onednn_md(res_flat), engine, res_flat.data_ptr());
      });

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;
  arg_handles.reserve(8);

  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1_.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();

  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);
  return result;
}

static at::Tensor dnnl_matmul_w4a16_and_add(
    Tensor& result, // dst, [b, M, N]
    const Tensor& mat1_, // src, [b, M, K]
    const Tensor& mat2, // quantized weight, [K/8, N]
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [K/group_size, N]
    const Tensor& zp, // [k/group_size, N/8]
    const Tensor& res,
    int64_t group_size,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION(
      "dnnl_matmul_w4a16_and_add", std::vector<c10::IValue>({mat1_, mat2}));

  auto mat1 = g_idx.has_value() ? mat1_.index_select(-1, g_idx.value()) : mat1_;
  auto o_sz = mat1.sizes().vec();
  auto b_sz = mat2.sizes();
  *(o_sz.end() - 1) = *(b_sz.end() - 1);
  result = at::empty(o_sz, mat1.options());

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device

  auto bias_add_int4 = [&](primitive_attr& pattr) {
    auto res_flat = res.flatten(0, -2);
    post_ops po;
    po.append_binary(algorithm::binary_add, get_onednn_md(res_flat));
    pattr.set_post_ops(po);
  };

  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1,
      mat2,
      std::nullopt,
      result,
      scale,
      zp,
      group_size,
      engine,
      bias_add_int4);

  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
          /* return dpcpp_onednn_memory(get_onednn_md(zp), engine,
           * zp.data_ptr());
           */
        });
  }

  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_MULTIPLE_POST_OP(0) | DNNL_ARG_SRC_1,
      res.data_ptr(),
      [&]() {
        auto res_flat = res.flatten(0, -2);
        return dpcpp_onednn_memory(
            get_onednn_md(res_flat), engine, res_flat.data_ptr());
      });

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;
  arg_handles.reserve(8);
  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias.has_value()) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();
  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);

  return result;
}

static at::Tensor dnnl_matmul_w4a16_and_bias_add(
    Tensor& result, // dst, [b, M, N]
    const Tensor& mat1_, // src, [b, M, K]
    const Tensor& mat2, // quantized weight, [K/8, N]
    const c10::optional<Tensor>& bias,
    const Tensor& scale, // [K/group_size, N]
    const Tensor& zp, // [k/group_size, N/8]
    const Tensor& res,
    int64_t group_size,
    bool m2_trans,
    const c10::optional<Tensor>& g_idx) {
  RECORD_FUNCTION(
      "dnnl_matmul_w4a16_and_bias_add",
      std::vector<c10::IValue>({mat1_, mat2}));

  auto mat1 = g_idx.has_value() ? mat1_.index_select(-1, g_idx.value()) : mat1_;
  auto o_sz = mat1.sizes().vec();
  auto b_sz = mat2.sizes();
  *(o_sz.end() - 1) = *(b_sz.end() - 1);
  result = at::empty(o_sz, mat1.options());

  // get device, engine, stream
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device

  auto bias_add_int4 = [&](primitive_attr& pattr) {
    auto res_flat = res.flatten(0, -2);
    post_ops po;
    po.append_binary(algorithm::binary_add, get_onednn_md(res_flat));
    pattr.set_post_ops(po);
  };

  auto& matmul_ext = dnnlMatmulCreatePrimitive(
      mat1, mat2, bias, result, scale, zp, group_size, engine, bias_add_int4);

  // set scale and zero point for matmul args
  int arg_off = 0;
  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS,
      scale.data_ptr(),
      [&]() {
        return dpcpp_onednn_memory(
            get_onednn_md(scale), engine, scale.data_ptr());
      });

  // set zp_md for symmetric quantization
  if (zp.dim() == 1) {
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          return dpcpp_onednn_memory(get_onednn_md(zp), engine, zp.data_ptr());
        });
  } else {
    // set zp_md for asymmetric quantization
    matmul_ext.set_attribute(
        arg_off++,
        DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS,
        zp.data_ptr(),
        [&]() {
          int m = mat1.sizes()[0];
          int n = mat2.sizes()[1];
          int k = mat1.sizes()[1];

          const uint64_t num_groups = (uint64_t)(k / group_size);
          memory zp_B_u4_m(
              {{num_groups, n}, memory::data_type::u4, {n, 1}},
              engine,
              zp.data_ptr());
          return zp_B_u4_m;
          /* return dpcpp_onednn_memory(get_onednn_md(zp), engine,
           * zp.data_ptr());
           */
        });
  }

  matmul_ext.set_attribute(
      arg_off++,
      DNNL_ARG_ATTR_MULTIPLE_POST_OP(0) | DNNL_ARG_SRC_1,
      res.data_ptr(),
      [&]() {
        auto res_flat = res.flatten(0, -2);
        return dpcpp_onednn_memory(
            get_onednn_md(res_flat), engine, res_flat.data_ptr());
      });

  // set general args
  std::vector<std::pair<int, void*>> arg_handles;
  arg_handles.reserve(8);

  arg_handles.emplace_back(DNNL_ARG_SRC, mat1.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_WEIGHTS, mat2.data_ptr());
  arg_handles.emplace_back(DNNL_ARG_DST, result.data_ptr());
  if (bias.has_value()) {
    arg_handles.emplace_back(DNNL_ARG_BIAS, bias.value().data_ptr());
  }

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_ext.get_scratchpad_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, mat1.options().dtype(at::kByte), c10::nullopt);
  arg_handles.emplace_back(DNNL_ARG_SCRATCHPAD, scratchpad_tensor.data_ptr());
#endif

  auto strm = GpuStreamManager::Instance().get_stream();
  /* matmul_ext.execute(strm, engine, std::move(arg_handles), arg_off); */
  DPCPP_ONEDNN_EXEC_WITH_ARGHANDLES(
      matmul_ext, strm, engine, arg_handles, arg_off);
  return result;
}

} // namespace oneDNN
} // namespace torch_ipex::xpu

#endif // USE_PRIMITIVE_CACHE
