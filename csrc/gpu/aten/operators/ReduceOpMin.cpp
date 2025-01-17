#include <ATen/Context.h>
#include <ATen/WrapDimUtils.h>
#include <ATen/WrapDimUtilsMulti.h>
#include <ATen/core/DimVector.h>
#include <ATen/native/ReduceOps.h>
#include <ATen/native/ReduceOpsUtils.h>
#include <ATen/native/SharedReduceOps.h>
#include <ATen/native/TensorIterator.h>

#include <c10/core/ScalarType.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Numerics.h"
#include "comm/RegistrationDeclarations.h"

#include "Reduce.h"
#include "ReduceOpsUtils.h"

using namespace torch_ipex::xpu::dpcpp;
using namespace at::native;

namespace at {
namespace AtenIpexTypeXPU {

template <typename acc_t>
struct ReduceMinOps {
  ReduceMinOps() {}
  acc_t operator()(acc_t a, acc_t b) const {
    return (Numerics<acc_t>::lt(a, b) || Numerics<acc_t>::isnan(a)) ? a : b;
  }
};

template <
    typename scalar_t,
    typename acc_t = scalar_t,
    typename out_t = scalar_t>
void min_kernel_impl(TensorIterator& iter) {
  dpcpp_reduce_kernel<scalar_t, out_t>(
      iter,
      func_wrapper<scalar_t>(ReduceMinOps<scalar_t>()),
      Numerics<scalar_t>::upper_bound());
}

static void min_kernel(TensorIterator& iter) {
  IPEX_DISPATCH_ALL_TYPES_AND3(
      at::ScalarType::Bool,
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.dtype(),
      "min",
      [&]() { min_kernel_impl<scalar_t>(iter); });
}

Tensor min(const Tensor& self) {
  Tensor result;
  ScalarType dtype = get_dtype(result, self, c10::nullopt);
  auto iter = meta::make_reduction(
      "min", result, self, std::vector<int64_t>{}, false, dtype);
  if (iter.numel() == 0) {
    result.zero_();
  } else {
    min_kernel(iter);
  }
  return result;
}

} // namespace AtenIpexTypeXPU
} // namespace at
