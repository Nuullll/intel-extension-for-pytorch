import torch
import torch.nn as nn
from torch.autograd import Variable
from torch.testing._internal.common_utils import TestCase
import torch_ipex

cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestNNMethod(TestCase):
    def test_batch_norm_half(self, dtype=torch.half):
        x_i = torch.randn([2, 2, 3, 3], device=cpu_device)
        x_dpcpp_i = x_i.to(dpcpp_device).to(dtype)

        bn = nn.BatchNorm2d(2)
        y_cpu = bn(x_i)
        bn.to(dpcpp_device).to(dtype)
        y_dpcpp = bn(x_dpcpp_i)
        self.assertEqual(y_cpu, y_dpcpp.cpu().float(), atol=1e-2, rtol=0)

    def test_batch_norm_bfloat16(self, dtype=torch.bfloat16):
        x_i = torch.randn([2, 2, 3, 3], device=cpu_device)
        grad_i = torch.randn([2, 2, 3, 3], device=cpu_device)

        x_dpcpp_i = x_i.to(dpcpp_device).to(dtype)
        grad_dpcpp_i = grad_i.to(dpcpp_device).to(dtype)

        x_cpu = Variable(x_i, requires_grad=True)
        grad_cpu = Variable(grad_i, requires_grad=True)
        bn = nn.BatchNorm2d(2)
        y_cpu = bn(x_cpu)

        y_cpu.backward(grad_cpu)

        print("x_cpu = ", y_cpu)
        print("x_cpu.grad = ", x_cpu.grad)

        x_dpcpp = Variable(x_dpcpp_i, requires_grad=True)
        grad_dpcpp = Variable(grad_dpcpp_i, requires_grad=True)
        bn.to(dtype).to(dpcpp_device)
        y_dpcpp = bn(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)

        print("y_dpcpp = ", y_dpcpp.cpu())
        print("x_dpcpp.grad", x_dpcpp.grad.cpu())
        self.assertEqual(y_cpu, y_dpcpp.to(cpu_device).float(), rtol=10e-4, atol=10e-2)
        self.assertEqual(x_cpu.grad, x_dpcpp.grad.to(cpu_device).float(), rtol=10e-4, atol=10e-2)

    def test_batch_norm(self, dtype=torch.float):

        x_i = torch.randn([2, 2, 3, 3], device=cpu_device)
        grad_i = torch.randn([2, 2, 3, 3], device=cpu_device)

        x_dpcpp_i = x_i.to(dpcpp_device)
        grad_dpcpp_i = grad_i.to(dpcpp_device)

        self.assertEqual(x_i, x_dpcpp_i.to(cpu_device))
        self.assertEqual(grad_i, grad_dpcpp_i.to(cpu_device))

        x_cpu = Variable(x_i, requires_grad=True)
        grad_cpu = Variable(grad_i, requires_grad=True)
        bn1 = nn.BatchNorm2d(2)
        bn2 = nn.BatchNorm2d(2)
        y_cpu1 = bn1(x_cpu)
        y_cpu = bn2(y_cpu1)

        y_cpu.backward(grad_cpu)

        print("x_cpu = ", y_cpu)
        print("x_cpu.grad = ", x_cpu.grad)

        x_dpcpp = Variable(x_dpcpp_i, requires_grad=True)
        grad_dpcpp = Variable(grad_dpcpp_i, requires_grad=True)
        bn1.to(dpcpp_device)
        bn2.to(dpcpp_device)

        y_dpcpp1 = bn1(x_dpcpp)
        y_dpcpp = bn2(y_dpcpp1)

        y_dpcpp.backward(grad_dpcpp)

        #y = y_dpcpp1.cpu()
        #y = Variable(y, requires_grad = True)
        # y.backward(grad_cpu)
        print("y_dpcpp = ", y_dpcpp.cpu())
        print("x_dpcpp.grad", x_dpcpp.grad.cpu())
        self.assertEqual(y_cpu, y_dpcpp.to(cpu_device))
        self.assertEqual(x_cpu.grad, x_dpcpp.grad.to(cpu_device))

    def test_channels_last_simple_fwd(self, dtype=torch.float):
        x = torch.randn(1, 2, 3, 3, dtype=torch.float)
        conv = torch.nn.Conv2d(2, 2, kernel_size=3, stride=1, padding=1, bias=False)
        bn = torch.nn.BatchNorm2d(2)

        relu = torch.nn.ReLU()
        ref = conv(x)
        ref = bn(ref)
        ref = relu(ref)

        x = x.to("xpu").to(memory_format=torch.channels_last)
        conv.to("xpu")
        bn.to("xpu")
        real = conv(x)
        real = bn(real)
        real = relu(real)
        real = real.contiguous().cpu()

        print(real)
        print(ref)
        self.assertEqual(real, ref)

    def test_channels_last_simple_bwd(self, dtype=torch.float):
        bn = nn.BatchNorm2d(2)
        x_i = torch.randn([2, 2, 3, 3], device=cpu_device)
        grad_i = torch.randn([2, 2, 3, 3], device=cpu_device)

        x_dpcpp_i = x_i.to(dpcpp_device).to(memory_format=torch.channels_last)
        grad_dpcpp_i = grad_i.to(dpcpp_device).to(memory_format=torch.channels_last)

        x_cpu = Variable(x_i, requires_grad=True)
        grad_cpu = Variable(grad_i, requires_grad=True)

        y_cpu1 = bn(x_cpu)
        y_cpu = bn(y_cpu1)

        y_cpu.backward(grad_cpu)

        print("x_cpu = ", y_cpu)
        print("x_cpu.grad = ", x_cpu.grad)

        x_dpcpp = Variable(x_dpcpp_i, requires_grad=True)
        grad_dpcpp = Variable(grad_dpcpp_i, requires_grad=True)
        bn.to(dpcpp_device)

        y_dpcpp1 = bn(x_dpcpp)
        y_dpcpp = bn(y_dpcpp1)

        y_dpcpp.backward(grad_dpcpp)


        print("y_dpcpp = ", y_dpcpp.cpu())
        print("x_dpcpp.grad", x_dpcpp.grad.cpu())
        self.assertEqual(y_cpu, y_dpcpp.to(cpu_device))
        self.assertEqual(x_cpu.grad, x_dpcpp.grad.to(cpu_device))