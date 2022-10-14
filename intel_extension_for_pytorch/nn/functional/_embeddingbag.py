import torch
import intel_extension_for_pytorch._C as core
import warnings
from typing import Optional, Tuple
Tensor = torch.Tensor

def _embedding_bag_fast_path_sum(
    weights: Tensor,
    mode: int = 0,
    scale_grad_by_freq: bool = False,
    per_sample_weights: Optional[Tensor] = None,
    padding_idx: Optional[int] = None
) -> bool:
    if mode != 0 or scale_grad_by_freq:
        return False
    if weights.stride(1) != 1 or weights.dtype not in (torch.float, torch.bfloat16):
        return False
    if per_sample_weights is not None or padding_idx is not None:
        return False
    return True

torch_embedding_bag = torch.embedding_bag

def _embeddingbag(
    weights: Tensor,
    indices: Tensor,
    offsets: Tensor,
    scale_grad_by_freq: bool = False,
    mode:int = 0,
    sparse: bool = False,
    per_sample_weights: Optional[Tensor] = None,
    include_last_offset: bool = False,
    padding_idx: Optional[int] = None
) -> Tuple[Tensor, Tensor, Tensor, Tensor]:
    if _embedding_bag_fast_path_sum(
        weights, mode, scale_grad_by_freq, per_sample_weights, padding_idx
    ):
        ret = torch.ops.torch_ipex.embedding_bag(weights, indices, offsets, sparse, include_last_offset)
        # torch.embedding_bag expected 4 Tensor returned
        # here we only return 1 tensor since the other three tensors are not needed in our fast path
        ret = (ret, torch.empty(0), torch.empty(0), torch.empty(0))
    else:
        warnings.warn('Fallback to torch.embedding bag')
        ret = torch_embedding_bag(weights, indices, offsets, scale_grad_by_freq, mode, sparse, per_sample_weights, include_last_offset, padding_idx)
    return ret

torch.embedding_bag = _embeddingbag