#include <ATen/ATen.h>
#include <ATen/native/TensorIterator.h>

#include <c10/util/MathConstants.h>
#include <cmath>
#include <limits>
#include "Loops.h"
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Numerics.h"
#include "comm/RegistrationDeclarations.h"

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <typename T>
static inline T polevl(const T x, const T A[], size_t len) {
  T result = 0;
  for (size_t i = 0; i <= len; i++) {
    result = result * x + A[i];
  }
  return result;
}

/*
 * This function is derived from the implementation of the digamma function in
 * the Cephes Math Library. See note [3-Clause BSD License for the Cephes Math
 * Library] in ATen/native/Math.h.
 */
template <typename scalar_t, typename accscalar_t>
static inline scalar_t digamma_one(scalar_t x) {
  constexpr accscalar_t PSI_10 = 2.25175258906672110764;
  if (x == 0) {
    return std::numeric_limits<scalar_t>::infinity();
  }
  accscalar_t additional_summand = 0;
  int x_is_integer = x == Numerics<scalar_t>::floor(x);
  if (x < 0) {
    if (x_is_integer) {
      return std::numeric_limits<scalar_t>::infinity();
    }
    additional_summand =
        -c10::pi<scalar_t> / Numerics<scalar_t>::tan(c10::pi<scalar_t> * x);
    x = 1 - x;
  }

  // Push x to be >= 10
  accscalar_t result = 0;
  while (x < 10) {
    result -= 1 / x;
    x += 1;
  }
  if (x == 10) {
    return result + PSI_10 + additional_summand;
  }

  // Compute asymptotic digamma
  static const accscalar_t A[] = {
      8.33333333333333333333E-2,
      -2.10927960927960927961E-2,
      7.57575757575757575758E-3,
      -4.16666666666666666667E-3,
      3.96825396825396825397E-3,
      -8.33333333333333333333E-3,
      8.33333333333333333333E-2,
  };

  accscalar_t y = 0;
  if (x < 1.0e17f) {
    accscalar_t z = 1.0 / (x * x);
    y = z * polevl<accscalar_t>(z, A, 6);
  }
  return static_cast<scalar_t>(
      result + Numerics<scalar_t>::log(x) - (0.5f / x) - y +
      additional_summand);
}

// Computes the reparameterized gradient -(d/dalpha cdf(x;alpha)) / pdf(x;alpha)
// for random number x drawn from a standard Gamma distribution Gamma(alpha).
template <typename scalar_t, typename accscalar_t>
scalar_t standard_gamma_grad_one(scalar_t alpha_, scalar_t x_) {
  // Use a Taylor series expansion for small x.
  accscalar_t x = static_cast<accscalar_t>(x_);
  accscalar_t alpha = static_cast<accscalar_t>(alpha_);
  if (x < 0.8f) {
    accscalar_t numer = 1;
    accscalar_t denom = alpha;
    auto series1 = numer / denom;
    auto series2 = numer / (denom * denom);
    for (int i = 1; i <= 5; ++i) {
      numer *= -x / static_cast<accscalar_t>(i);
      denom += 1;
      series1 += numer / denom;
      series2 += numer / (denom * denom);
    }
    const auto pow_x_alpha = Numerics<accscalar_t>::pow(x, alpha);
    const auto gamma_pdf = Numerics<accscalar_t>::pow(x, alpha - 1) *
        Numerics<accscalar_t>::exp(-x);
    const auto gamma_cdf = pow_x_alpha * series1;
    const auto gamma_cdf_alpha =
        (Numerics<accscalar_t>::log(x) -
         digamma_one<accscalar_t, accscalar_t>(alpha)) *
            gamma_cdf -
        pow_x_alpha * series2;
    const auto result = -gamma_cdf_alpha / gamma_pdf;
    return std::isnan(result) ? static_cast<scalar_t>(0.f)
                              : static_cast<scalar_t>(result);
  }

  // Use a Rice saddle point expansion for large alpha.
  if (alpha > 8.0f) {
    if (0.9f * alpha <= x && x <= 1.1f * alpha) {
      const auto numer_1 = 1 + 24 * alpha * (1 + 12 * alpha);
      const auto numer_2 = 1440 * (alpha * alpha) + 6 * x * (53 - 120 * x) -
          65 * x * x / alpha + alpha * (107 + 3600 * x);
      const auto denom = 1244160 * (alpha * alpha) * (alpha * alpha);
      return static_cast<scalar_t>(numer_1 * numer_2 / denom);
    }
    const auto denom = Numerics<accscalar_t>::sqrt(8 * alpha);
    const auto term2 = denom / (alpha - x);
    const auto term3 = Numerics<accscalar_t>::pow(
        x - alpha - alpha * Numerics<accscalar_t>::log(x / alpha),
        static_cast<accscalar_t>(-1.5));
    const auto term23 = (x < alpha) ? term2 - term3 : term2 + term3;
    const auto term1 = Numerics<accscalar_t>::log(x / alpha) * term23 -
        Numerics<accscalar_t>::sqrt(2 / alpha) * (alpha + x) /
            ((alpha - x) * (alpha - x));
    const auto stirling = 1 + 1 / (12 * alpha) * (1 + 1 / (24 * alpha));
    const auto numer = x * term1;
    return static_cast<scalar_t>(-stirling * numer / denom);
  }

  // Use a bivariate rational approximation to the reparameterized gradient.
  const auto u = Numerics<accscalar_t>::log(x / alpha);
  const auto v = Numerics<accscalar_t>::log(alpha);
  static const accscalar_t coef_uv[3][8] = {
      {0.16009398,
       -0.094634809,
       0.025146376,
       -0.0030648343,
       1,
       0.32668115,
       0.10406089,
       0.0014179084},
      {0.53487893,
       0.1298071,
       0.065735949,
       -0.0015649758,
       0.16639465,
       0.020070113,
       -0.0035938915,
       -0.00058392623},
      {0.040121004,
       -0.0065914022,
       -0.0026286047,
       -0.0013441777,
       0.017050642,
       -0.0021309326,
       0.00085092367,
       -1.5247877e-07},
  };
  accscalar_t coef_v[8];
  for (int i = 0; i < 8; ++i) {
    coef_v[i] = coef_uv[0][i] + u * (coef_uv[1][i] + u * coef_uv[2][i]);
  }
  const auto p = coef_v[0] + v * (coef_v[1] + v * (coef_v[2] + v * coef_v[3]));
  const auto q = coef_v[4] + v * (coef_v[5] + v * (coef_v[6] + v * coef_v[7]));
  return static_cast<scalar_t>(Numerics<accscalar_t>::exp(p / q));
}

} // namespace impl

template <typename scalar_t, typename accscalar_t>
struct _standard_gamma_grad_functor {
  scalar_t operator()(scalar_t self_val, scalar_t output_val) const {
    return impl::standard_gamma_grad_one<scalar_t, accscalar_t>(
        self_val, output_val);
  }
};

Tensor _standard_gamma_grad(const Tensor& self, const Tensor& output) {
  Tensor ret = at::empty(self.sizes(), self.options());
  TensorIterator iter = TensorIteratorConfig()
                            .add_output(ret)
                            .add_input(self)
                            .add_input(output)
                            .build();
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "_standard_gamma_grad",
      [&] {
        using accscalar_t = acc_type<scalar_t>;
        _standard_gamma_grad_functor<scalar_t, accscalar_t> f;
        dpcpp_fast_mode_kernel_for_tensor_iter(iter, f);
      });
  return ret;
}
} // namespace AtenIpexTypeXPU
} // namespace at