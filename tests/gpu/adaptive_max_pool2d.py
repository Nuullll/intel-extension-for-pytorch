import torch
import torch.nn as nn

import torch_ipex

dtype = torch.float
cpu_device = torch.device("cpu")
sycl_device = torch.device("dpcpp")

x_cpu = torch.randn([1, 1, 8, 8], device=cpu_device, dtype=dtype)
grad_cpu = torch.randn([1, 1, 2, 2], device=cpu_device, dtype=dtype)
x_sycl = x_cpu.to("dpcpp")
grad_sycl = grad_cpu.to("dpcpp")

max_pool = nn.AdaptiveMaxPool2d((2,2), return_indices=True)


x_cpu.requires_grad_(True)
y_cpu = max_pool(x_cpu)
print("y_cpu", y_cpu[0])
output_cpu = y_cpu[0].backward(grad_cpu)
print("x_cpu.grad", x_cpu.grad)

x_sycl.requires_grad_(True)
max_pool = max_pool.to("dpcpp")
y_sycl = max_pool(x_sycl)
print("y_sycl", y_sycl[0].cpu())
output_sycl = y_sycl[0].backward(grad_sycl)
print("x_sycl.grad", x_sycl.grad.cpu())