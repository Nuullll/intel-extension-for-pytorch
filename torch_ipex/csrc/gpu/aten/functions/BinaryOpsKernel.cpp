#include <ATen/Context.h>
#include <ATen/Dispatch.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/BinaryOps.h>

#include <functions/Loops.h>


namespace at { namespace native {

//Note: sycl compiler does not support uname type in template.
class SyclOpAdd{};
class SyclOpMul{};
class SyclOpDiv{};

static void add_kernel_sycl(TensorIterator& iter, Scalar alpha_scalar) {
  AT_DISPATCH_ALL_TYPES_AND(at::ScalarType::Half, iter.dtype(), "add", [&]() {
    auto alpha = alpha_scalar.to<scalar_t> ();
    sycl_kernel_for_tensor_iter<SyclOpAdd>(iter,
        [=](scalar_t a, scalar_t b) -> scalar_t {
          return a + alpha * b;
        });
  });
}

static void sub_kernel_sycl(TensorIterator& iter, Scalar alpha_scalar) {
  return add_kernel_sycl(iter, -alpha_scalar);
}

static void mul_kernel_sycl(TensorIterator& iter) {
  AT_DISPATCH_ALL_TYPES_AND(at::ScalarType::Half, iter.dtype(), "mul", [&]() {
    sycl_kernel_for_tensor_iter<SyclOpMul>(iter,
        [=](scalar_t a, scalar_t b) -> scalar_t {
          return a * b;
        });
  });
}

static void div_kernel_sycl(TensorIterator& iter) {
  if (isIntegralType(iter.dtype(), false)) {
    AT_DISPATCH_INTEGRAL_TYPES(iter.dtype(), "div", [&] {
      sycl_kernel_for_tensor_iter<SyclOpDiv>(iter,
        [](scalar_t a, scalar_t b)-> scalar_t {
        return a / b;
      });
    });
  } else {
    AT_DISPATCH_FLOATING_TYPES(iter.dtype(), "div", [&]() {
      sycl_kernel_for_tensor_iter<SyclOpDiv>(iter,
        [](scalar_t a, scalar_t b)-> scalar_t {
        return a / b;
      });
    });
  }
}

}
}

namespace at { namespace AtenIpexTypeDPCPP {

//alpha_check
static inline void alpha_check(const TensorIterator& iter, Scalar alpha) {
  AT_CHECK(! alpha.isBoolean() || iter.dtype() == ScalarType::Bool,
              "Boolean alpha only supported for Boolean results.");
  AT_CHECK(isFloatingType(iter.dtype()) || alpha.isIntegral(true),
              "For integral input tensors, argument alpha must not be a floating point number.");
}

//scalar to tensor
static Tensor wrapped_scalar_tensor(Scalar scalar) {
  auto tensor = scalar_to_tensor(scalar);
  tensor.unsafeGetTensorImpl()->set_wrapped_number(true);
  return tensor;
}

Tensor& add_out(Tensor& result, const Tensor& self, const Tensor& other, Scalar alpha) {
  auto iter = TensorIterator::binary_op(result, self, other,
    /*check_mem_overlap=*/true);
  alpha_check(iter, alpha);
  at::native::add_kernel_sycl(iter,alpha);
  TORCH_INTERNAL_ASSERT(result.scalar_type() == iter.output().dtype());
  return result;
}

Tensor add(const Tensor& self, const Tensor& other, Scalar alpha) {
  Tensor result;
  auto iter = TensorIterator::binary_op(result, self, other);
  alpha_check(iter, alpha);
  at::native::add_kernel_sycl(iter,alpha);
  return iter.output();
}

Tensor& add_(Tensor& self, const Tensor& other, Scalar alpha) {
  return at::AtenIpexTypeDPCPP::add_out(self, self, other, alpha);
}

Tensor add(const Tensor& self, Scalar other, Scalar alpha) {
  return at::AtenIpexTypeDPCPP::add(self, wrapped_scalar_tensor(other), alpha);
}

Tensor& add_(Tensor& self, Scalar other, Scalar alpha) {
  return at::AtenIpexTypeDPCPP::add_(self, wrapped_scalar_tensor(other), alpha);
}

// Basic checking for all sub functions.
static inline void sub_check(const Tensor& self, const Tensor& other) {
  AT_CHECK(self.scalar_type() != kBool || other.scalar_type() != kBool,
              "Subtraction, the `-` operator, with two bool tensors is not supported. "
              "Use the `^` or `logical_xor()` operator instead.");
  AT_CHECK(self.scalar_type() != kBool && other.scalar_type() != kBool,
              "Subtraction, the `-` operator, with a bool tensor is not supported. "
              "If you are trying to invert a mask, use the `~` or `logical_not()` operator instead.");
}

Tensor& sub_out(Tensor& result, const Tensor& self, const Tensor& other, Scalar alpha) {
  sub_check(self, other);
  auto iter = TensorIterator::binary_op(result, self, other,
    /*check_mem_overlap=*/true);
  alpha_check(iter, alpha);
  at::native::sub_kernel_sycl(iter,alpha);
  TORCH_INTERNAL_ASSERT(result.scalar_type() == iter.output().dtype());
  return result;
}

Tensor sub(const Tensor& self, const Tensor& other, Scalar alpha) {
  sub_check(self, other);
  Tensor result;
  auto iter = TensorIterator::binary_op(result, self, other);
  alpha_check(iter, alpha);
  at::native::sub_kernel_sycl(iter,alpha);
  return iter.output();
}

Tensor& sub_(Tensor& self, const Tensor& other, Scalar alpha) {
  return at::AtenIpexTypeDPCPP::sub_out(self, self, other, alpha);
}

Tensor rsub(const Tensor& self, const Tensor& other, Scalar alpha) {
  return at::AtenIpexTypeDPCPP::sub(other, self, alpha);
}

Tensor sub(const Tensor& self, Scalar other, Scalar alpha) {
  return at::AtenIpexTypeDPCPP::sub(self, wrapped_scalar_tensor(other), alpha);
}

Tensor& sub_(Tensor& self, Scalar other, Scalar alpha) {
  return at::AtenIpexTypeDPCPP::sub_(self, wrapped_scalar_tensor(other), alpha);
}

Tensor rsub(const Tensor& self, Scalar other, Scalar alpha) {
  return at::AtenIpexTypeDPCPP::rsub(self, wrapped_scalar_tensor(other), alpha);
}

Tensor& mul_out(Tensor& result, const Tensor& self, const Tensor& other) {
  auto iter = TensorIterator::binary_op(result, self, other,
    /*check_mem_overlap=*/true);
  at::native::mul_kernel_sycl(iter);
  return result;
}

Tensor mul(const Tensor& self, const Tensor& other) {
  Tensor result;
  auto iter = TensorIterator::binary_op(result, self, other);
  at::native::mul_kernel_sycl(iter);
  return iter.output();
}

Tensor& mul_(Tensor& self, const Tensor& other) {
  return at::AtenIpexTypeDPCPP::mul_out(self, self, other);
}

Tensor mul(const Tensor& self, Scalar other) {
  return at::AtenIpexTypeDPCPP::mul(self, wrapped_scalar_tensor(other));
}

Tensor& mul_(Tensor& self, Scalar other) {
  return at::AtenIpexTypeDPCPP::mul_(self, wrapped_scalar_tensor(other));
}

Tensor& div_out(Tensor& result, const Tensor& self, const Tensor& other) {
  auto iter = TensorIterator::binary_op(result, self, other,
    /*check_mem_overlap=*/true);
  at::native::div_kernel_sycl(iter);
  return result;
}

Tensor div(const Tensor& self, const Tensor& other) {
  Tensor result;
  auto iter = TensorIterator::binary_op(result, self, other);
  at::native::div_kernel_sycl(iter);
  return iter.output();
}

Tensor& div_(Tensor& self, const Tensor& other) {
  return at::AtenIpexTypeDPCPP::div_out(self, self, other);
}

} // namespace AtenIpexTypeDPCPP
} // namespace at