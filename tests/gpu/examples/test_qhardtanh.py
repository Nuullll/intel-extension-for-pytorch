import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


class TestTorchMethod(TestCase):
    def test_qhardtanh(self, dtype=torch.float):
        zp_vec = [0, 2]
        for dtype in [torch.qint8, torch.quint8]:
            for zp in zp_vec:
                inputs = torch.randn(5, 5)

                q_inputs = torch.quantize_per_tensor(inputs, 0.4, zp, dtype)

                output_int8 = torch.nn.quantized.functional.hardtanh(q_inputs)

                print("start xpu")
                inputs_gpu = inputs.to("xpu")

                q_inputs_gpu = torch.quantize_per_tensor(inputs_gpu, 0.4, zp, dtype)

                output_gpu_int8 = torch.nn.quantized.functional.hardtanh(q_inputs_gpu)

                self.assertEqual(output_int8, output_gpu_int8)


if __name__ == "__main__":
    mod = TestTorchMethod()
    mod.test_qhardtanh()
