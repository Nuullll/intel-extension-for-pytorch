import torch
import intel_extension_for_pytorch as ipex  # noqa
import pytest
from intel_extension_for_pytorch.nn.utils._quantize_convert import GPTQShuffle

from torch.testing._internal.common_utils import (
    TestCase,
    instantiate_parametrized_tests,
    parametrize,
    run_tests,
)

from enum import Enum
from typing import List


class QuantMode(Enum):
    SYM = 1
    ASYM = 2


AWQ_PACK_ORDER = [0, 2, 4, 6, 1, 3, 5, 7]
REVERSE_AWQ_PACK_ORDER = [0, 4, 1, 5, 2, 6, 3, 7]

checking_atol = 1e-2
checking_rtol = 1e-2
skip_bf16_input = not torch.xpu.has_2d_block_array() and not torch.xpu.has_xmx()


class TestOneDNNInt4Linear(TestCase):

    @staticmethod
    def unpack_weight(qweight, scales, qzeros, q_config):
        group_size = q_config["group_size"]
        bits = q_config["bits"]
        s32_bits = 32

        assert bits == 4
        # Int32 can store 8 * 4bits data. This is the offset for each data.
        wf = (
            torch.tensor(list(range(0, s32_bits, bits)), dtype=torch.int32)
            .unsqueeze(0)
            .to("xpu")
        )
        zeros = qzeros
        if qzeros is not None:
            zeros = torch.bitwise_right_shift(
                torch.unsqueeze(qzeros, 2).expand(-1, -1, 32 // bits), wf.unsqueeze(0)
            ).to(torch.int16 if bits == 8 else torch.int8)
            torch.bitwise_and(zeros, (2**bits) - 1, out=zeros)

            zeros = zeros.reshape(scales.shape)

        weight = torch.bitwise_right_shift(
            torch.unsqueeze(qweight, 1).expand(-1, 32 // bits, -1), wf.unsqueeze(-1)
        ).to(torch.int16 if bits == 8 else torch.int8)
        torch.bitwise_and(weight, (2**bits) - 1, out=weight)

        return weight, scales, zeros

    @staticmethod
    def dequantize(qweight, scales, qzeros, group_size, g_idx=None):
        q_config = {"group_size": group_size, "bits": 4}
        weight, gptq_scales, gptq_zeros = TestOneDNNInt4Linear.unpack_weight(
            qweight, scales, qzeros, q_config
        )
        # gptq_zeros = (torch.ones_like(gptq_zeros) * 8).to("xpu")  # hard code zp
        if len(weight.shape) > 2:
            weight = weight.reshape(-1, weight.shape[-1])
        infeatures = weight.shape[0]
        if g_idx is None:
            g_idx = torch.tensor(
                [i // q_config["group_size"] for i in range(infeatures)],
                dtype=torch.int32,
            )
        if gptq_zeros is None:
            return (weight - 8) * gptq_scales[g_idx]
        else:
            return (weight - gptq_zeros[g_idx]) * gptq_scales[g_idx]

    @staticmethod
    def rand_int4(size, dtype=torch.int32, device="xpu"):
        rand = torch.randint(-128, 128, [size // 2], device=device).to(torch.int8)
        return rand.view(dtype=dtype)

    @parametrize("per_channel", [False], lambda k: "per_channel" * k)
    @parametrize("dtype", [torch.float16])
    @parametrize("qmode", list(QuantMode), lambda k: k.name)
    @parametrize("act_order", [False, True])
    @parametrize("m,n,k", [(8, 4096, 4096), (1, 4096, 11008), (32, 4096, 4096)])
    @pytest.mark.skipif(not torch.xpu.has_xetla(), reason="fallback is required")
    def test_gemm_int4(self, m, n, k, per_channel, act_order, qmode: QuantMode, dtype):
        input = torch.rand([m, k], device="xpu", dtype=dtype)
        input_torch = input.cpu()
        weight = self.rand_int4(k * n, torch.int32, "xpu").reshape(k // 8, n)

        group_size = min(128, k)
        if per_channel:
            group_size = k
        group_num = int(k / group_size)

        scales = torch.rand([group_num, n], device="xpu", dtype=dtype)
        if qmode == QuantMode.SYM:
            zero_points = None
        elif qmode == QuantMode.ASYM:
            zero_points = self.rand_int4(group_num * n, torch.int32, "xpu").reshape(
                group_num, n // 8
            )

        if act_order:
            g_idx = torch.randperm(k, dtype=torch.int32) // group_size
            shuf_weight = GPTQShuffle(bits=4, blocksize=group_size)
            shuffled_weight, g_idx4kernel = shuf_weight(weight, g_idx)
        else:
            g_idx = None
            g_idx4kernel = None
            shuffled_weight = weight

        # check fp16 gemm
        weight_fp = self.dequantize(
            weight, scales, zero_points, group_size, g_idx
        ).cpu()
        out_torch = torch.matmul(input_torch, weight_fp)

        # xetla int4 gemm
        # with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.XETLA):
        #     out_xetla = torch.ops.torch_ipex.mm_int4(
        #         input,
        #         shuffled_weight.t().contiguous(),
        #         scales.t().contiguous(),
        #         zero_points,
        #         group_size,
        #         g_idx4kernel,
        #     )

        # onednn int4 gemm
        weight_ba = shuffled_weight.transpose(0, 1).contiguous().transpose(0, 1)
        if qmode == QuantMode.SYM:
            zero_points = torch.Tensor([8]).to(torch.int8).to("xpu")
        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            out_onednn = torch.ops.torch_ipex.mm_int4(
                input, weight_ba, scales, zero_points, group_size, g_idx4kernel
            )

        self.assertEqual(
            out_onednn.cpu().float(),
            out_torch.cpu().float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )
        # check gemm + residual
        res0 = torch.rand([m, n], device="xpu", dtype=dtype)
        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            out_onednn_res = torch.ops.torch_ipex.mm_add_int4(
                input, weight_ba, scales, zero_points, group_size, res0, g_idx4kernel
            )
        out_torch_res = out_torch + res0.cpu().float()
        self.assertEqual(
            out_onednn_res.cpu().float(),
            out_torch_res.float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )

        # check gemm + bias
        bias = torch.rand([1, n], device="xpu", dtype=dtype)
        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            out_onednn_bias = torch.ops.torch_ipex.mm_bias_int4(
                input, weight_ba, bias, scales, zero_points, group_size, g_idx4kernel
            )
        out_torch_bias = out_torch + bias.cpu().float()
        self.assertEqual(
            out_onednn_bias.cpu().float(),
            out_torch_bias.float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )

        # check gemm + bias + gelu
        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            out_onednn_gelu = torch.ops.torch_ipex.mm_bias_gelu_int4(
                input,
                weight_ba,
                scales,
                zero_points,
                bias,
                group_size,
                "tanh",
                g_idx4kernel,
            )
        gelu_out = torch.nn.GELU(approximate="tanh")(out_torch_bias)
        self.assertEqual(
            out_onednn_gelu.cpu().float(),
            gelu_out.float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )

        # check gemm + silu + mul
        res0 = torch.rand([m, n], device="xpu", dtype=dtype)
        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            out_onednn_silu = torch.ops.torch_ipex.mm_silu_mul_int4(
                input, weight_ba, scales, zero_points, group_size, res0, g_idx4kernel
            )
        silu_mul_out = torch.nn.SiLU()(out_torch) * res0.cpu().float()
        self.assertEqual(
            out_onednn_silu.cpu().float(),
            silu_mul_out.float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )

        # check gemm + bias + residual + residual
        res0 = torch.rand([m, n], device="xpu", dtype=dtype)
        res1 = torch.rand([m, n], device="xpu", dtype=dtype)
        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            out_onednn_bias_2res = torch.ops.torch_ipex.mm_bias_resadd_resadd_int4(
                input,
                weight_ba,
                bias,
                res0,
                res1,
                scales,
                zero_points,
                group_size,
                g_idx4kernel,
            )
        out_torch_bias_2res = out_torch_bias + res0.cpu().float() + res1.cpu().float()
        self.assertEqual(
            out_onednn_bias_2res.cpu().float(),
            out_torch_bias_2res.float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )

        # check gemm + bias + residual
        res0 = torch.rand([m, n], device="xpu", dtype=dtype)
        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            out_onednn_bias_add = torch.ops.torch_ipex.mm_bias_add_int4(
                input,
                weight_ba,
                bias,
                scales,
                zero_points,
                group_size,
                res0,
                g_idx4kernel,
            )
        out_torch_bias_add = out_torch_bias + res0.cpu().float()
        self.assertEqual(
            out_onednn_bias_add.cpu().float(),
            out_torch_bias_add.float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )

    @staticmethod
    def pack(imatrix: torch.Tensor, direction: str = "column"):
        """
        Packs a 4-bit integer matrix into a packed 32-bit integer matrix.
        Args:
            imatrix (torch.Tensor): matrix of integers
            direction (str): direction of packing, either "column" or "row"
        Returns:
            qmatrix (torch.Tensor): packed matrix of integers
        """
        shifts = torch.arange(0, 32, 4, dtype=torch.int32, device=imatrix.device)

        imatrix = imatrix.to(torch.int8) & 0x0F  # eventually correct overflow

        if direction == "column":
            imatrix = imatrix.view(-1, imatrix.shape[1] // (32 // 4), (32 // 4))
            qmatrix = torch.bitwise_left_shift(imatrix, shifts[None, None, :]).sum(
                dim=-1
            )

        elif direction == "row":
            imatrix = imatrix.view(imatrix.shape[0] // (32 // 4), (32 // 4), -1)
            qmatrix = torch.bitwise_left_shift(imatrix, shifts[None, :, None]).sum(
                dim=1
            )

        qmatrix = qmatrix.to(torch.int32)

        return qmatrix

    @staticmethod
    def unpack(qmatrix: torch.Tensor, direction: str = "column"):
        """
        Unpacks a 32-bit packed integer matrix into a 4-bit integer matrix.
        Args:
            qmatrix (torch.Tensor): matrix of packed integers
            direction (str): direction of unpacking, either "column" or "row"
        Returns:
            imatrix (torch.Tensor): matrix of integers
        """
        shifts = torch.arange(0, 32, 4, device=qmatrix.device)

        if direction == "column":
            imatrix = torch.bitwise_right_shift(
                qmatrix[:, :, None], shifts[None, None, :]
            ).view(qmatrix.shape[0], -1)

        elif direction == "row":
            imatrix = torch.bitwise_right_shift(
                qmatrix[:, None, :], shifts[None, :, None]
            ).view(-1, qmatrix.shape[-1])

        imatrix = imatrix.to(torch.int8) & 0x0F  # eventually correct overflow

        return imatrix

    @staticmethod
    def apply_order(
        imatrix: torch.Tensor,
        direction: str = "column",
        order: List[int] = AWQ_PACK_ORDER,
    ):
        """
        Applies the order to a 4-bit integer matrix.
        Args:
            imatrix (torch.Tensor): matrix of integers
            direction (str): direction of applying order, either "column" or "row"
            order (List[int]): order to apply, default is AWQ_PACK_ORDER
        Returns:
            imatrix (torch.Tensor): matrix of integers
        """
        if direction == "column":
            imatrix = imatrix.view(-1, (32 // 4))[:, order].view(imatrix.shape)
        elif direction == "row":
            imatrix = imatrix.view((32 // 4), -1)[order, :].view(imatrix.shape)

        return imatrix

    @staticmethod
    def fast_awq_to_gptq(qweight, qzeros):
        # awq uses column packing for both weights and zeros
        izeros = TestOneDNNInt4Linear.unpack(qzeros, direction="column")
        iweights = TestOneDNNInt4Linear.unpack(qweight, direction="column")

        # Reverse the order of the iweight and izeros tensors
        izeros = TestOneDNNInt4Linear.apply_order(
            izeros, direction="column", order=REVERSE_AWQ_PACK_ORDER
        )
        iweights = TestOneDNNInt4Linear.apply_order(
            iweights, direction="column", order=REVERSE_AWQ_PACK_ORDER
        )

        # exllama uses row packing for weights and column packing for zeros
        qzeros = TestOneDNNInt4Linear.pack(izeros, direction="column")
        qweight = TestOneDNNInt4Linear.pack(iweights, direction="row")

        return qweight, qzeros

    @staticmethod
    def fast_gptq_to_awq(qweight, qzeros):
        # gptq uses row packing for both weights and zeros
        izeros = TestOneDNNInt4Linear.unpack(qzeros, direction="column")
        iweight = TestOneDNNInt4Linear.unpack(qweight, direction="row")

        izeros = TestOneDNNInt4Linear.apply_order(
            izeros, direction="column", order=AWQ_PACK_ORDER
        )
        iweight = TestOneDNNInt4Linear.apply_order(
            iweight, direction="row", order=AWQ_PACK_ORDER
        )

        # izeros = izeros + 1

        qzeros = TestOneDNNInt4Linear.pack(izeros, direction="column")
        qweight = TestOneDNNInt4Linear.pack(iweight, direction="column")

        return qweight, qzeros

    @parametrize("per_channel", [False], lambda k: "per_channel" * k)
    @parametrize("dtype", [torch.float16])
    @parametrize("act_order", [False, True])
    @parametrize("m,n,k", [(8, 4096, 4096), (1, 4096, 11008), (32, 4096, 4096)])
    @pytest.mark.skipif(not torch.xpu.has_xetla(), reason="fallback is required")
    def test_gptq_woqlinear_interface(
        self, m, n, k, per_channel, act_order, dtype=torch.float16
    ):
        input = torch.rand([m, k], device="xpu", dtype=dtype)
        input_torch = input.cpu()
        weight = self.rand_int4(k * n, torch.int32, "xpu").reshape(k // 8, n)
        group_size = min(128, k)

        g_idx = None
        if act_order:
            g_idx = torch.randperm(k, dtype=torch.int32) // group_size
        if per_channel:
            group_size = k
        group_num = int(k / group_size)

        scales = -torch.rand([group_num, n], device="xpu", dtype=dtype)
        zero_points = None
        zero_points_kernel = None

        weight_fp16 = self.dequantize(
            weight, scales, zero_points, group_size, g_idx
        ).cpu()

        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            woqlinear = ipex.llm.quantization.IPEXWeightOnlyQuantizedLinear.from_weight(
                weight,
                scales,
                zero_points_kernel,
                k,
                n,
                None,
                None,
                group_size,
                g_idx.to("xpu") if g_idx is not None else None,
                ipex.llm.quantization.QuantMethod.GPTQ_GEMM,
                ipex.llm.quantization.QuantDtype.INT4,
            )
            out_onednn = woqlinear(input)
        out_torch = torch.matmul(input_torch, weight_fp16)
        self.assertEqual(
            out_onednn.cpu().float(),
            out_torch.float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )

    @parametrize("per_channel", [False], lambda k: "per_channel" * k)
    @parametrize("dtype", [torch.float16])
    @parametrize("act_order", [False])
    @parametrize("m,n,k", [(8, 4096, 4096), (1, 4096, 11008), (32, 4096, 4096)])
    @pytest.mark.skipif(not torch.xpu.has_xetla(), reason="fallback is required")
    def test_awq_woqlinear_interface(
        self, m, n, k, per_channel, act_order, dtype=torch.float16
    ):
        input = torch.rand([m, k], device="xpu", dtype=dtype)
        input_torch = input.cpu()
        weight = self.rand_int4(k * n, torch.int32, "xpu").reshape(k, n // 8)
        group_size = min(128, k)

        g_idx = None
        g_idx4kernel = None
        if per_channel:
            group_size = k
        group_num = int(k / group_size)

        scales = -torch.rand([group_num, n], device="xpu", dtype=dtype)
        zero_points = self.rand_int4(group_num * n, torch.int32, "xpu").reshape(
            group_num, n // 8
        )

        weight_gptq, zero_points_gptq = self.fast_awq_to_gptq(weight, zero_points)

        weight_fp16 = self.dequantize(
            weight_gptq, scales, zero_points_gptq, group_size, g_idx
        ).cpu()
        with torch.xpu.compute_eng(torch.xpu.XPUComputeEng.ONEDNN):
            woqlinear = ipex.llm.quantization.IPEXWeightOnlyQuantizedLinear.from_weight(
                weight,
                scales,
                zero_points,
                k,
                n,
                None,
                None,
                group_size,
                g_idx4kernel,
                ipex.llm.quantization.QuantMethod.AWQ_GEMM,
                ipex.llm.quantization.QuantDtype.INT4,
            )
            out_onednn = woqlinear(input)
        out_torch = torch.matmul(input_torch, weight_fp16)
        self.assertEqual(
            out_onednn.cpu().float(),
            out_torch.float(),
            atol=checking_atol,
            rtol=checking_rtol,
        )


instantiate_parametrized_tests(TestOneDNNInt4Linear)

if __name__ == "__main__":
    run_tests()
