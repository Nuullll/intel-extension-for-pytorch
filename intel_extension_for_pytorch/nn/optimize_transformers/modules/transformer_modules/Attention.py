import torch
import torch.nn as nn
import torch.distributed as dist
from typing import Optional, Tuple, Union
from .Activation import ACT2FN
from .._transformer_configuration import IPEXTransformerConfig
import os
import math
from dataclasses import dataclass
from .NaiveAttention import IPEXTransformerAttnNaive
from .BaseAttention import IPEXTransformerAttn
from .Linear import matmul_add_add



@dataclass
class IPEXRuntimeAttnCache:
    key_cache:   torch.Tensor = None
    value_cache: torch.Tensor = None
    key_prompt:  torch.Tensor = None
    value_prompt:torch.Tensor = None

    def clear_cache(self):
        self.key_cache    = None
        self.value_cache  = None
        self.key_prompt   = None
        self.value_prompt = None


class IPEXTransformerAttnOptimizedFp16(IPEXTransformerAttnNaive):
    blocked_alibi = None
    blocked_attn_mask = None
    casual_attention_mask = None


    def __init__(self,
                 config: IPEXTransformerConfig) -> None:
        super().__init__(config)
        self.config = config
        self.runtime_cache = IPEXRuntimeAttnCache()

    def release_resources(self):
        self.runtime_cache.clear_cache()

# ################################ pre_qkv ######################################################
    def pre_qkv(self, hidden_states, key_value_states, layer_past, **kwargs):
        if self.is_beam_search():
            self.prepare_cache_for_beam_search(hidden_states, layer_past)
        else:
            self.prepare_cache_for_greedy_search(hidden_states, layer_past)


    def prepare_cache_for_greedy_search(self, hidden_states, layer_past):
        bs_beam, seq_len, _ = self.get_runtime_shape(hidden_states)
        self.prepare_kv_cache(hidden_states)
        self.prev_seq_len = layer_past[0].size(2) if layer_past is not None else 0
        self.seq_len = self.prev_seq_len + 1 if self.prev_seq_len != 0 else seq_len

    def prepare_cache_for_beam_search(self, hidden_states, layer_past):
        self.prepare_kv_prompt(hidden_states)
        self.prepare_kv_cache(hidden_states)
        if self.is_1st_token_beam_search():
            self.prev_seq_len = 0
            self.seq_len = 0
        else:
            self.seq_len = self.prev_seq_len + 1

    def prepare_kv_prompt(self, hidden_states):
        if self.runtime_cache.key_prompt is None or self.runtime_cache.value_prompt is None or IPEXTransformerAttn.timestamp == 0:
            self.runtime_cache.key_prompt = torch.empty_like(hidden_states)
            self.runtime_cache.value_prompt = torch.empty_like(hidden_states)

    def prepare_kv_cache(self, hidden_states):
        bs_beam, seq_len, embed_dim = self.get_runtime_shape(hidden_states)
        batch_size = bs_beam // self.beam_size

        if self.runtime_cache.key_cache is None or self.runtime_cache.value_cache is None or batch_size != self.batch_size:
            cache_shape = [self.max_position, bs_beam, self.num_attn_head, self.head_dim]
            self.runtime_cache.key_cache = torch.empty(cache_shape, device=hidden_states.device, dtype=hidden_states.dtype)
            self.runtime_cache.value_cache = torch.empty(cache_shape, device=hidden_states.device, dtype=hidden_states.dtype)
            self.batch_size = batch_size

# ##############################################################################################

# ################################ qkv_gemm #####################################################
    def qkv_gemm(self, hidden_states, key_value_states, layer_past, **kwargs):
        query, key, value = self.prepare_qkv_input(hidden_states)
        query, key, value = self.compute_qkv_gemm(hidden_states, query, key, value)
        query, key, value = self.process_qkv_output(hidden_states, query, key, value)
        return query, key, value

# ################################ prepare_qkv_input ###########################################

    def prepare_qkv_input(self, hidden_states, **kwargs):
        # assert False, "prepare_qkv_input() in Attention.py have not been properly dispatched during runtime"
        if self.is_1st_token_beam_search():
            return self.prepare_qkv_input_1st_token_beam_search(hidden_states)
        else:
            return self.prepare_qkv_input_2nd2last(hidden_states)

    def prepare_qkv_input_1st_token_beam_search(self, hidden_states, **kwargs):
        bs_beam, seq_len, embed_dim = self.get_runtime_shape(hidden_states)
        out_shape = [bs_beam, seq_len, self.head_dim * self.num_attn_head]
        query = torch.empty(out_shape, device=hidden_states.device, dtype=hidden_states.dtype)
        return query, self.runtime_cache.key_prompt, self.runtime_cache.value_prompt

    def prepare_qkv_input_2nd2last(self, hidden_states, **kwargs):
        bs_beam, seq_len, embed_dim = self.get_runtime_shape(hidden_states)
        out_shape = [seq_len, bs_beam, self.head_dim * self.num_attn_head]
        query = torch.empty(out_shape, device=hidden_states.device, dtype=hidden_states.dtype)
        key = self.runtime_cache.key_cache[self.prev_seq_len:self.seq_len, :, :, :].view(out_shape)
        value = self.runtime_cache.value_cache[self.prev_seq_len:self.seq_len, :, :, :].view(out_shape)
        return query, key, value

# ################################# compute_qkv #################################################

    def compute_qkv_gemm(self, hidden_states, query, key, value):
        torch.ops.torch_ipex.mm_qkv_out(hidden_states, self.qkv_proj.weight, self.qkv_proj.bias, query, key, value)
        return query, key, value

# ################################# process_qkv_output ###########################################
    def process_qkv_output(self, hidden_states, query, key, value):
        out_shape = hidden_states.size()[:-1] + (self.num_attn_head, self.head_dim)
        if self.is_1st_token_beam_search():
            self.runtime_cache.key_prompt = self.runtime_cache.key_prompt.view(out_shape)
            self.runtime_cache.value_prompt = self.runtime_cache.value_prompt.view(out_shape)
        if self.is_beam_search():
            self.prev_seq_len = self.seq_len
        query = query.view(out_shape)
        key = key.view(out_shape)
        value = value.view(out_shape)
        return query, key, value

# ################################## post_qkv ##########################################################################
    def post_qkv(self, query, key, value, position_ids, layer_past, **kwargs):
        bs_beam, seq, _ = self.get_runtime_shape(query)
        query, key = self.position_embed(query, key, position_ids, self.layer_id, self.beam_size, seq)
        query, key, value = self.combine_kv_cache_interface(query, key, value)
        return query, key, value

# ################################## combine_kv_cache ################################################################
    def combine_kv_cache_interface(self, query, key, value, layer_past=None):
        if self.is_1st_token_beam_search():
            return self.combine_kv_cache_1st_token_beam_search(query, key, value, layer_past)
        else:
            return self.combine_kv_cache_2nd2last(query, key, value, layer_past)

    def combine_kv_cache_1st_token_beam_search(self, query, key, value, layer_past=None):
        self.runtime_cache.key_prompt = self.runtime_cache.key_prompt.permute(0, 2, 1, 3)
        self.runtime_cache.value_prompt = self.runtime_cache.value_prompt.permute(0, 2, 1, 3)
        query = query.permute(0, 2, 1, 3)
        return query, self.runtime_cache.key_prompt, self.runtime_cache.value_prompt

    def combine_kv_cache_2nd2last(self, query, key, value, layer_past=None):
        key = self.runtime_cache.key_cache[:self.seq_len, :, :, :]
        value = self.runtime_cache.value_cache[:self.seq_len, :, :, :]
        query = query.permute(1, 2, 0, 3)
        key = key.permute(1, 2, 0, 3)
        value = value.permute(1, 2, 0, 3)
        # key, value = self.cat_past_kv(key, value, layer_past)
        return query, key, value

# ################################################### pre sdp ###################################################
    def pre_sdp(self,key, value):
        return key, value

    def sdp_kv_preprocess(self, key, value):
        if self.is_1st_token_beam_search():
            return self.sdp_kv_preprocess_1st_token_beam_search(key, value)
        else:
            return self.sdp_kv_preprocess_2nd2last(key, value)

    def sdp_kv_preprocess_1st_token_beam_search(self, key, value):
        key = self.repeat_kv(key, 1)
        value = self.repeat_kv(value, 1)
        key_prompt, value_prompt = key, value
        return key, value, key_prompt, value_prompt

    def sdp_kv_preprocess_2nd2last(self, key, value):
        key = self.repeat_kv(key, 1)
        value = self.repeat_kv(value, 1)
        key_prompt = self.repeat_kv(self.runtime_cache.key_prompt, 1)
        value_prompt = self.repeat_kv(self.runtime_cache.value_prompt, 1)
        return key, value, key_prompt, value_prompt



# ################################################################ sdp #######################################################################################
    def sdp(self, query, key, value, attention_mask, head_mask, alibi):
        key, value, key_prompt, value_prompt = self.sdp_kv_preprocess(key, value)
        dropout, alpha, beta, is_casual, blocked_attn_mask, blocked_alibi = self.prepare_sdp_input(query, key, value, attention_mask, alibi)
        attention_output, attn_weight = self.compute_sdp(query, key, value, key_prompt, value_prompt, blocked_attn_mask, blocked_alibi, head_mask, alpha, beta, dropout, is_casual)
        attention_output = self.process_sdp_output(attention_output)
        attention_output = attention_output.reshape(attention_output.size()[:-2] + (self.head_dim * self.num_attn_head,))
        return attention_output, attn_weight

    def prepare_sdp_input(self, query, key, value, attention_mask, alibi):
        dropout = 0.0
        alpha=  1.0 / math.sqrt(self.head_dim)
        beta = 1.0
        is_causal = False
        if self.use_casual_mask == True and query.shape[2] != 1:
            is_causal = True
        blocked_attn_mask = None
        if attention_mask != None:
            if attention_mask.dtype == torch.bool:
                blocked_attn_mask = None
                if query.shape[2] != 1:
                    is_causal = True
            else:
                blocked_attn_mask = self.get_blocked_attn_mask(attention_mask)
        blocked_alibi = None
        if alibi is not None:
            blocked_alibi = self.get_blocked_alibi(alibi)

        return dropout, alpha, beta, is_causal, blocked_attn_mask, blocked_alibi

    def compute_sdp(self, query, key, value, key_prompt, value_prompt, attention_mask, alibi, head_mask, alpha, beta, dropout, is_causal):
        if self.is_1st_token_beam_search():
            return self.sdp_1st_token_beam_search(query, key, value, key_prompt, value_prompt, attention_mask, alibi, head_mask, alpha, beta, dropout, is_causal)
        elif self.is_beam_search():
            return self.sdp_2nd2last_beam_search(query, key, value, key_prompt, value_prompt, attention_mask, alibi, head_mask, alpha, beta, dropout, is_causal)
        else:
            return self.sdp_greedy_search(query, key, value, key_prompt, value_prompt, attention_mask, alibi, head_mask, alpha, beta, dropout, is_causal)

    def sdp_greedy_search(self, query, key, value, key_prompt, value_prompt, attention_mask, alibi, head_mask, alpha, beta, dropout, is_causal):
        attention_output = torch.xpu.IpexSDP(query, key, value, alibi, attention_mask, head_mask, alpha, beta, dropout, is_causal, True)
        return attention_output, None

    def sdp_1st_token_beam_search(self, query, key, value, key_prompt, value_prompt, attention_mask, alibi, head_mask, alpha, beta, dropout, is_causal):
        attention_output = torch.xpu.IpexSDP(query, key, value, alibi, attention_mask, head_mask, alpha, beta, dropout, is_causal, False)
        return attention_output, None

    def sdp_2nd2last_beam_search(self, query, key, value, key_prompt, value_prompt, attention_mask, alibi, head_mask, alpha, beta, dropout, is_causal):
        attention_output = torch.xpu.IpexSDP_Index(query, key_prompt, value_prompt, key, value, self.beam_idx, alibi, attention_mask, head_mask, self.seq_len, alpha, beta, dropout, is_causal)
        return attention_output, None

    def process_sdp_output(self, attention_output):
        if self.is_1st_token_beam_search():
            return self.process_sdp_output_1st_token_beam_search(attention_output)
        else:
            return self.process_sdp_output_general(attention_output)

    def process_sdp_output_1st_token_beam_search(self, attention_output):
        return attention_output.permute(0, 2, 1, 3)

    def process_sdp_output_general(self, attention_output):
        return attention_output.permute(2, 0, 1, 3)

# ######################################################################### post sdp #########################################################################

    def post_sdp(self, attn_output, residual=None):
        attn_output = self.out_proj_compute(attn_output, residual)
        self.all_reduce_if_necessary(attn_output)
        return attn_output

    def out_proj_compute(self, attn_output, residual=None):
        shape = [attn_output.shape[0], attn_output.shape[1], self.embed_dim]
        attn_output = matmul_add_add(attn_output, self.out_proj.weight, self.tp_size, self.out_proj.bias, residual)
        attn_output = attn_output.view(shape)
        return attn_output


    def get_present(self, query, key, value, use_cache):
        if use_cache or self.is_decoder:
            if not self.is_beam_search():
                present = (key, value)
            else:
                # key, value shape [bs*beam=1, head, seq, dim]
                seq_len = self.seq_len if self.runtime_cache.key_prompt is None else self.seq_len + self.runtime_cache.key_prompt.shape[2]
                cache_shape = (1, key.shape[1], seq_len, key.shape[3])
                key_cache = torch.empty(cache_shape, device=key.device, dtype=key.dtype)
                value_cache = key_cache
                present = (key_cache, value_cache)
        else:
            present = None
        return present
# ###################################################################################################################################################################

    def all_reduce_if_necessary(self, reduce_target):
        if self.tp_group is not None:
            dist.all_reduce(reduce_target, group=self.tp_group)
        return reduce_target

    def get_blocked_alibi(self, alibi):
        if self.layer_id == 0:
            shape = [alibi.shape[0], alibi.shape[1], self.max_position] # [beam*num_head, q_len, kv_len]
            IPEXTransformerAttnOptimizedFp16.blocked_alibi = torch.empty(shape, device=alibi.device, dtype=alibi.dtype)
            kv_len = alibi.shape[2]
            IPEXTransformerAttnOptimizedFp16.blocked_alibi[:, :, 0 : kv_len] = alibi
        return IPEXTransformerAttnOptimizedFp16.blocked_alibi

    def get_blocked_attn_mask(self, attn_mask):
        if self.layer_id == 0:
            IPEXTransformerAttnOptimizedFp16.blocked_attn_mask = torch.empty((attn_mask.shape[0], attn_mask.shape[1], attn_mask.shape[2], self.max_position), device=attn_mask.device, dtype=attn_mask.dtype)
            IPEXTransformerAttnOptimizedFp16.blocked_attn_mask.fill_(-65504.)
            IPEXTransformerAttnOptimizedFp16.blocked_attn_mask[:, :, :, 0 : attn_mask.shape[3]] = attn_mask
        return IPEXTransformerAttnOptimizedFp16.blocked_attn_mask

    def repeat_kv(self, kv, n_rep):
        if n_rep == 1:
            return kv
        bs_beam, num_kv_heads, seq_len, head_dim = kv.shape
        if IPEXTransformerAttn.timestamp == 0 and self.beam_size > 1:
            kv = kv.permute(0, 2, 1, 3)
            kv = kv[:, :, :, None, :].expand(bs_beam, seq_len, num_kv_heads, n_rep, head_dim)
            kv = kv.reshape(bs_beam, seq_len, num_kv_heads * n_rep, head_dim)
            kv = kv.permute(1, 2, 0, 3)
        else:
            kv = kv.permute(2, 0, 1, 3)
            kv = kv[:, :, :, None, :].expand(seq_len, bs_beam, num_kv_heads, n_rep, head_dim)
            kv = kv.reshape(seq_len, bs_beam, num_kv_heads * n_rep, head_dim)
            kv = kv.permute(1, 2, 0, 3)
        return kv


    def get_runtime_shape(self, hidden_states):
        # This api should always return the shape attr in [bs * beam, seq_len, num_head * head_dim]
        if self.is_1st_token_beam_search():
            return hidden_states.shape[0], hidden_states.shape[1], hidden_states.shape[2]
        else:
            return hidden_states.shape[1], hidden_states.shape[0], hidden_states.shape[2]


    def is_first_token_beam_search(self):
        return self.beam_size > 1 and IPEXTransformerAttn.timestamp == 0
    
    def is_beam_search(self):
        return self.beam_size > 1

    def is_naive_implementation():
        return False

