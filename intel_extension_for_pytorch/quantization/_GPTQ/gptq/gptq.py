from ....utils._logger import logger, WarningType
import math
import random
import re
import time
import torch
import torch.nn as nn
import transformers
from functools import partial
from .model_utils import (
    find_layers,
    trace_gptq_target_blocks,
    log_quantizable_layers_per_transformer,
    move_input_to_device,
    quantize,
)

DEBUG = False


class GPTQuantizer(object):
    """Main API for GPTQ algorithm."""

    def __init__(
        self,
        model,
        weight_config=None,
        dataloader=None,
        nsamples=128,
        use_max_length=True,
        pad_max_length=2048,
        device=torch.device("cpu"),
        layer_wise=False,
        cache_positional_arguments=None,
    ):
        """
        Args:
            model (torch.nn.Module): fp32 model to quantize
            weight_config (dict, optional): contains all info required by GPTQ. Defaults to {}.
                For example:
                weight_config={
                    'layer1':
                    {
                        'bits': 4,
                        'group_size': 32,
                        'sym': False,
                        'percdamp': .01,
                        'act_order': False
                    }
                    ...
                }
            dataloader (torch.utils.data.DataLoader): an iterable containing calibration datasets.
            nsamples (int): calibration samples' size.
            use_max_length (bool): whether to align calibration data to a fixed length.
            pad_max_length (int): whether to align calibration data to a fixed length.
            device: set to torch.device("cpu").
            layer_wise (bool): whether to do LWQ.
        """
        self.model = model
        self.gptq_related_blocks = trace_gptq_target_blocks(self.model)
        self.dtype = next(iter(self.model.parameters())).dtype
        log_quantizable_layers_per_transformer(self.gptq_related_blocks)

        if weight_config is None:
            weight_config = {}
        self.weight_config = weight_config
        # default settings, check configs
        self.wbits_default = 4
        self.group_size_default = 128
        self.block_size_default = 128
        self.percdamp_default = 0.01
        self.sym_default = False
        self.act_order_default = False
        self.perchannel_default = True
        self.mse_default = False
        self.check_layer_config()
        self.device = torch.device("cpu")
        self.layer_wise = layer_wise
        self.is_ready = False
        self.cache_positional_arguments = cache_positional_arguments

        # dataloader
        self.use_max_length = use_max_length
        self.pad_max_length = pad_max_length
        self.dataloader_original = dataloader
        self.dataloader = []
        self.nsamples = nsamples
        self.prepare_dataloader()

    def prepare_dataloader(self):
        if self.use_max_length:
            # (Recommend) only take sequence whose length exceeds self.pad_max_length,
            # which preserves calibration's tokens are all valid
            # This is GPTQ official dataloader implementation
            self.obtain_first_n_samples_fulllength()
        else:
            # general selection, no padding, not GPTQ original implementation.
            self.obtain_first_n_samples()
        self.cache_key_arguments = {
            "i": 0
        }  # a dict of list, keyword arguments ("attention_masks", "position_ids", etc.)
        # Note that the first elements in cache_positional_arguments is main input: hidden_states
        if self.cache_positional_arguments is None:
            self.cache_positional_arguments = (
                []
            )  # a list of list, positional arguments ("rotary_pos_emb" in chatglm)
        self.is_ready = True

    def obtain_first_n_samples(self, seed=0):
        """Get first nsample data as the real calibration dataset."""
        self.dataloader.clear()
        random.seed(seed)
        for batch in self.dataloader_original:
            # process data, depends on its data type.
            if len(self.dataloader) == self.nsamples:
                logger.info(
                    f"Successfully collect {self.nsamples} calibration samples."
                )
                break
            if isinstance(batch, list) or isinstance(batch, tuple):
                if batch[0].shape[-1] > self.pad_max_length:
                    i = random.randint(0, batch[0].shape[-1] - self.pad_max_length - 1)
                    j = i + self.pad_max_length
                    batch_final = []
                    for item in batch:
                        if isinstance(item, torch.Tensor) and item.shape.__len__() == 2:
                            batch_final.append(item[:, i:j])
                        else:
                            batch_final.append(item)
                else:
                    batch_final = batch[:]
            elif isinstance(batch, dict):
                try:
                    length = batch["input_ids"].shape[-1]
                except Exception:
                    logger.warning(
                        "Please make sure your dict'like data contains key of 'input_ids'.",
                        _type=WarningType.WrongArgument,
                    )
                    continue
                batch_final = {}
                if length > self.pad_max_length:
                    i = random.randint(0, length - self.pad_max_length - 1)
                    j = i + self.pad_max_length
                    # may have to slice every sequence related data
                    for key in batch.keys():
                        if isinstance(batch[key], torch.Tensor):
                            batch_final[key] = batch[key][
                                :, i:j
                            ]  # slice on sequence length dim
                        else:
                            batch_final[key] = batch[key]
                else:
                    batch_final = batch
            # tensor
            else:
                if batch.shape[-1] > self.pad_max_length:
                    i = random.randint(0, batch.shape[-1] - self.pad_max_length - 1)
                    j = i + self.pad_max_length
                    batch_final = batch[:, i:j]
                else:
                    batch_final = batch
            self.dataloader.append(batch_final)
        if len(self.dataloader) < self.nsamples:
            logger.warning(
                f"Try to use {self.nsamples} data, but entire dataset size is {len(self.dataloader)}.",
                _type=WarningType.WrongArgument,
            )

    def obtain_first_n_samples_fulllength(self, seed=0):
        self.dataloader.clear()
        random.seed(seed)
        unified_length = self.pad_max_length
        for batch in self.dataloader_original:
            if len(self.dataloader) == self.nsamples:
                logger.info(
                    f"Successfully collect {self.nsamples} calibration samples."
                )
                break
            if isinstance(batch, list) or isinstance(batch, tuple):
                if batch[0].shape[-1] == unified_length:
                    batch_final = batch[:]
                elif batch[0].shape[-1] > unified_length:
                    i = random.randint(0, batch[0].shape[-1] - unified_length - 1)
                    j = i + unified_length
                    batch_final = []
                    for item in batch:
                        if isinstance(item, torch.Tensor) and item.shape.__len__() == 2:
                            batch_final.append(item[:, i:j])
                        else:
                            batch_final.append(item)
                else:
                    # not match max length, not include in target dataset
                    continue
            elif isinstance(batch, dict):
                try:
                    length = batch["input_ids"].shape[-1]
                except Exception:
                    logger.warning(
                        "Please make sure your dict'like data contains key of 'input_ids'.",
                        _type=WarningType.WrongArgument,
                    )
                    continue
                batch_final = {}
                if length == self.pad_max_length:
                    batch_final = batch
                elif length > self.pad_max_length:
                    i = random.randint(0, length - self.pad_max_length - 1)
                    j = i + self.pad_max_length
                    # may have to slice every sequence related data
                    for key in batch.keys():
                        if isinstance(batch[key], torch.Tensor):
                            batch_final[key] = batch[key][
                                :, i:j
                            ]  # slice on sequence length dim with same position
                        else:
                            batch_final[key] = batch[key]
                else:
                    # not match max length, not include in target dataset
                    continue
            # tensor
            else:
                if batch.shape[-1] == unified_length:
                    batch_final = batch
                elif batch.shape[-1] > unified_length:
                    i = random.randint(0, batch.shape[-1] - unified_length - 1)
                    j = i + unified_length
                    batch_final = batch[:, i:j]
                else:
                    # not match max length, not include in target dataset
                    continue
            self.dataloader.append(batch_final)
        if len(self.dataloader) < self.nsamples:  # pragma: no cover
            logger.warning(
                f"Trying to allocate {self.nsamples} data with fixed length {unified_length},"
                + f"but only {len(self.dataloader)} samples are found. Please use smaller 'self.pad_max_length' value.",
                _type=WarningType.WrongArgument,
            )

    def get_full_layer_name(self, sub_layer_name, block_idx):
        transformer_name = self.gptq_related_blocks["transformers_name"]
        return ".".join([transformer_name, str(block_idx), sub_layer_name])

    def check_layer_config(self):
        """Copy arguments from weight_config to built-in attributes."""
        if "wbits" in self.weight_config:
            tmp_weight_config = {}
            for name, module in self.model.named_modules():
                tmp_weight_config[name] = {}
                tmp_weight_config[name]["wbits"] = self.weight_config.get(
                    "wbits", self.wbits_default
                )
                tmp_weight_config[name]["group_size"] = self.weight_config.get(
                    "group_size", self.group_size_default
                )
                tmp_weight_config[name]["block_size"] = self.weight_config.get(
                    "block_size", self.group_size_default
                )
                tmp_weight_config[name]["percdamp"] = self.weight_config.get(
                    "pecdamp", self.percdamp_default
                )
                tmp_weight_config[name]["sym"] = self.weight_config.get(
                    "sym", self.sym_default
                )
                tmp_weight_config[name]["act_order"] = self.weight_config.get(
                    "act_order", self.act_order_default
                )
                tmp_weight_config[name]["perchannel"] = self.weight_config.get(
                    "perchannel", self.perchannel_default
                )
                tmp_weight_config[name]["mse"] = self.weight_config.get(
                    "mse", self.mse_default
                )
            self.weight_config = tmp_weight_config
        else:
            for layer_name, config in self.weight_config.items():
                self.weight_config[layer_name]["wbits"] = config.get(
                    "wbits", self.wbits_default
                )
                self.weight_config[layer_name]["group_size"] = config.get(
                    "group_size", self.group_size_default
                )
                self.weight_config[layer_name]["block_size"] = config.get(
                    "block_size", self.group_size_default
                )
                self.weight_config[layer_name]["percdamp"] = config.get(
                    "pecdamp", self.percdamp_default
                )
                self.weight_config[layer_name]["sym"] = config.get(
                    "sym", self.sym_default
                )
                self.weight_config[layer_name]["act_order"] = config.get(
                    "act_order", self.act_order_default
                )
                self.weight_config[layer_name]["perchannel"] = config.get(
                    "perchannel", self.perchannel_default
                )
                self.weight_config[layer_name]["mse"] = config.get(
                    "mse", self.mse_default
                )

    def get_layer_config(self, layer_name):
        """Obtain config for one layer, since GPTQ supports layer-wise config."""
        # First try the exact name matching, if cannot find, use re to search. For example, can support ".*" in op_name
        config = None
        config = self.weight_config.get(layer_name, None)
        if config is not None:
            return config
        else:
            for k, v in self.weight_config.items():
                regex = re.compile(k)
                if len(regex.findall(layer_name)) is not None:
                    config = v
                    return config
                else:
                    pass
        return config

    def track_hidden_states(self, data):
        if isinstance(data, torch.Tensor):
            return data
        elif isinstance(data, tuple) or isinstance(data, list):
            return data[0]

    @torch.no_grad()
    def pre_quantization(self):
        """Prepare input calibration data and other attributes which are critical for gptq execution."""

        # critical: hooker function which collects inputs
        def forward(layer, *args, **kwargs):
            self.cache_key_arguments["i"] += 1
            for arg in kwargs:
                # each outputs can be different shape, hence also use list to store
                if isinstance(kwargs[arg], torch.Tensor) or arg == "alibi":
                    if self.cache_key_arguments.get(arg, None) is None:
                        self.cache_key_arguments[arg] = []
                    self.cache_key_arguments[arg].append(kwargs[arg])
                continue
            # copy positional arguments, positional arguments are sensitive for their order, be cautious!
            # Most models in HF has avoid this, but some models still use positional arguments other than
            # hidden_states, chatglm2-6b etc.
            for idx, item in enumerate(args):
                if (idx + 1) > len(self.cache_positional_arguments):
                    # initialize
                    self.cache_positional_arguments.append([])
                self.cache_positional_arguments[idx].append(item)
            raise ValueError

        # Step1: fetch the embeddings and other layers before the transformer stack.
        if not self.layer_wise:
            for embedding_name, embedding_layer in self.gptq_related_blocks[
                "embeddings"
            ].items():
                embedding_layer = embedding_layer.to(self.device)

        # Step2: modify the first transformer block's forward function to obtain inputs for calibration
        if not self.layer_wise:
            self.gptq_related_blocks["transformers"][0] = self.gptq_related_blocks[
                "transformers"
            ][0].to(self.device)
        forward_cache = self.gptq_related_blocks["transformers"][0].forward
        self.gptq_related_blocks["transformers"][0].forward = partial(
            forward, self.gptq_related_blocks["transformers"][0]
        )

        # Step3: run forward to obtain calibration datasets
        logger.info("Collecting calibration inputs...")
        for batch in self.dataloader:
            if not self.layer_wise:
                batch = move_input_to_device(batch, self.device)
            try:
                if isinstance(batch, tuple) or isinstance(batch, list):
                    self.model(batch[0])
                elif isinstance(batch, dict):
                    self.model(**batch)
                else:
                    self.model(batch)
            except ValueError:
                pass
        # output inp data shape
        logger.info("All calibration data's shape =>")
        # check all hidden_states shape
        try:
            for hidden_states in self.cache_positional_arguments[0]:
                logger.info(hidden_states.shape)
        except Exception:
            pass
        logger.info("Done.")

        # Step 4: restore original forward function, relocate layers back to cpu.
        self.gptq_related_blocks["transformers"][0].forward = forward_cache
        if not self.layer_wise:
            self.gptq_related_blocks["transformers"][0] = self.gptq_related_blocks[
                "transformers"
            ][0].cpu()
            for embedding_name, embedding_layer in self.gptq_related_blocks[
                "embeddings"
            ].items():
                embedding_layer.to(self.device)
        torch.cuda.empty_cache()
        # End
        logger.info("GPTQ quantization prepared.")

    def gather_single_batch_from_dict(self, data_dict, idx):
        # obtain a set of keyword input from cache
        single_batch = {}
        for k, v in data_dict.items():
            single_batch[k] = data_dict[k][idx]
        return single_batch

    def gather_single_batch_from_list(self, data_list, idx):
        # obtain a set of keyword input from cache
        single_batch = []
        for data_item in data_list:
            single_batch.append(data_item[idx])
        return single_batch

    def update_blockwise_hidden_states(self, outs):
        if "hidden_states" in self.cache_key_arguments:
            self.cache_key_arguments["hidden_states"] = outs[:]
        else:
            self.cache_positional_arguments[0] = outs[:]

    @torch.no_grad()
    def execute_quantization(self, means=None, stds=None, model_path=None):
        """Run quantization."""
        # Step1: prepare quantization (calibration datasets)
        logger.info("Begin ====>")
        self.pre_quantization()

        # Step2: run gptq quantization in a transformer block-wise manner.
        gptq_config = {}
        tblock_length = len(self.gptq_related_blocks["transformers"])
        for block_idx in range(tblock_length):
            logger.info(f"Quantizing layer {block_idx + 1} / {tblock_length}..")
            if not self.layer_wise:
                # if we do not apply layer-wise feature, we still place the entire block on the GPU
                transformer_block = self.gptq_related_blocks["transformers"][
                    block_idx
                ].to(self.device)
            else:
                transformer_block = self.gptq_related_blocks["transformers"][block_idx]
            # Step2.1: obtain all layers (Linear, Conv2d, etc) in the block which can be quantized.
            sub_layers = find_layers(transformer_block)
            sub_layers_to_quant = {}
            for layer_name, layer_obj in sub_layers.items():
                # filter sub_layers with included layer_names in self.weight_config
                full_layer_name = self.get_full_layer_name(layer_name, block_idx)
                if self.get_layer_config(full_layer_name) is None:
                    logger.warning(
                        f"{full_layer_name} can be quantized "
                        + "but excluded from quantization configs."
                    )
                else:
                    sub_layers_to_quant[layer_name] = layer_obj
            del sub_layers
            sub_layers = sub_layers_to_quant
            # Step 2.2: Initialize GPTQ quantizers for collected layers.
            gptq_for_this_block = {}
            # initialize gptq quantizer for every layer in a transformer block
            for layer_name in sub_layers:
                full_layer_name = self.get_full_layer_name(layer_name, block_idx)
                weight_config_this_layer = self.get_layer_config(full_layer_name)
                W = sub_layers[layer_name].weight.data.clone()

                gptq_for_this_block[layer_name] = GPTQ(
                    sub_layers[layer_name], W, self.device
                )
                gptq_for_this_block[layer_name].quantizer.configure(
                    weight_config_this_layer["wbits"],
                    weight_config_this_layer["perchannel"],
                    weight_config_this_layer["sym"],
                    weight_config_this_layer["mse"],
                )

            # Step 2.3: modify forward functions to hook inputs data (used in gptq execution)
            def add_batch(_name):
                def tmp(_, inp, out):
                    gptq_for_this_block[_name].add_batch(
                        inp[0].data, out.data
                    )  # noqa: F821

                return tmp

            handles = []  # register handles which add inputs and outputs to gptq object
            for layer_name in sub_layers:
                handles.append(
                    sub_layers[layer_name].register_forward_hook(add_batch(layer_name))
                )
            idx = self.cache_key_arguments.pop("i")
            for j in range(len(self.dataloader)):
                cache_keyword_batch = self.gather_single_batch_from_dict(
                    self.cache_key_arguments, j
                )
                cache_positional_batch = self.gather_single_batch_from_list(
                    self.cache_positional_arguments, j
                )
                out = transformer_block(*cache_positional_batch, **cache_keyword_batch)
                out = self.track_hidden_states(out)
            self.cache_key_arguments["i"] = idx
            for h in handles:
                h.remove()
            # Step 2.4: everything is prepared, so start quantization!
            for layer_name in sub_layers:
                weight_config_this_layer = self.get_layer_config(
                    self.get_full_layer_name(layer_name, block_idx)
                )
                logger.info(f"Quantizing layer {layer_name}")
                W = sub_layers[layer_name].weight.data.clone()
                scale, zp, Q = gptq_for_this_block[layer_name].fasterquant(
                    W,
                    blocksize=weight_config_this_layer["block_size"],
                    percdamp=weight_config_this_layer["percdamp"],
                    groupsize=weight_config_this_layer["group_size"],
                    act_order=weight_config_this_layer["act_order"],
                )
                sub_layers[layer_name].weight.data = Q
                gptq_config[self.get_full_layer_name(layer_name, block_idx)] = {
                    "scale": scale
                }
                if not weight_config_this_layer["sym"]:
                    gptq_config[self.get_full_layer_name(layer_name, block_idx)][
                        "zero"
                    ] = zp
                if weight_config_this_layer[
                    "act_order"
                ]:  # save perm for restoring the weights
                    gptq_config[self.get_full_layer_name(layer_name, block_idx)][
                        "perm"
                    ] = gptq_for_this_block[layer_name].perm
                gptq_for_this_block[layer_name].free()

            # Step 2.5: replace output data with quantized weights
            outs = []
            idx = self.cache_key_arguments.pop("i")
            for j in range(len(self.dataloader)):
                cache_keyword_batch = self.gather_single_batch_from_dict(
                    self.cache_key_arguments, j
                )
                cache_positional_batch = self.gather_single_batch_from_list(
                    self.cache_positional_arguments, j
                )
                out = transformer_block(*cache_positional_batch, **cache_keyword_batch)
                out = self.track_hidden_states(out)
                outs.append(out)
            self.cache_key_arguments["i"] = idx
            if self.layer_wise:
                self.gptq_related_blocks["transformers"][block_idx] = transformer_block
            else:
                self.gptq_related_blocks["transformers"][
                    block_idx
                ] = transformer_block.cpu()
            del gptq_for_this_block
            torch.cuda.empty_cache()
            # iteratively replace the input with output, thus layerwise quantization can continue.
            self.update_blockwise_hidden_states(outs)
            logger.info("------------------------------")

        logger.info("Quantization done")

        # obtain model
        for k, v in gptq_config.items():
            for m, n in v.items():
                gptq_config[k][m] = n.tolist()
        return self.model, gptq_config


class GPTQ:
    """
    Please refer to:
    GPTQ: Accurate Post-training Compression for Generative Pretrained Transformers (https://arxiv.org/abs/2210.17323)
    """

    def __init__(self, layer, W, device="cpu"):
        self.layer = layer
        self.device = device
        if isinstance(self.layer, nn.Conv2d) or isinstance(self.layer, nn.Conv1d):
            W = W.flatten(1)
        if isinstance(self.layer, transformers.Conv1D):
            W = W.t()
        self.rows = W.shape[0]  # output channels
        self.columns = W.shape[1]  # input channels
        self.H = torch.zeros((self.columns, self.columns), device=self.device)
        self.nsamples = 0
        self.quantizer = Quantizer()
        self.perm = None  # act_order choice

    def add_batch(self, inp, out):
        if len(inp.shape) == 2:
            inp = inp.unsqueeze(0)
        tmp = inp.shape[0]
        if isinstance(self.layer, nn.Linear) or isinstance(
            self.layer, transformers.Conv1D
        ):
            if len(inp.shape) == 3:
                inp = inp.reshape((-1, inp.shape[-1]))
            inp = inp.t()
        # TODO: llm's transformer sequential with nn.conv2d is currently not under test
        # if isinstance(self.layer, nn.Conv2d):
        #     unfold = nn.Unfold(
        #         self.layer.kernel_size,
        #         dilation=self.layer.dilation,
        #         padding=self.layer.padding,
        #         stride=self.layer.stride
        #     )
        #     inp = unfold(inp)
        #     inp = inp.permute([1, 0, 2])
        #     inp = inp.flatten(1)
        self.H *= self.nsamples / (self.nsamples + tmp)
        self.nsamples += tmp
        inp = math.sqrt(2 / self.nsamples) * inp.float()
        self.H += inp.matmul(inp.t())  # H = X*X, which should be a sysm matrix

    def fasterquant(
        self, W, blocksize=128, percdamp=0.01, groupsize=-1, act_order=False
    ):
        weight_shape, weight_dtype = W.shape, W.data.dtype
        if isinstance(self.layer, nn.Conv2d):
            W = W.flatten(1)
        if isinstance(self.layer, transformers.Conv1D):
            W = W.t()
        W = W.float()

        tick = time.time()

        if not self.quantizer.ready():
            self.quantizer.find_params(W, weight=True)

        H = self.H
        del self.H
        dead = torch.diag(H) == 0
        H[dead, dead] = 1
        W[:, dead] = 0  # such channel makes no contribution to quantization computation

        # rearrange considering the diag's value
        if act_order:
            perm = torch.argsort(torch.diag(H), descending=True)
            W = W[:, perm]
            H = H[perm][:, perm]
            self.perm = perm.clone()

        Losses = torch.zeros_like(W)
        Q = torch.zeros_like(W)

        damp = percdamp * torch.mean(torch.diag(H))
        diag = torch.arange(self.columns, device=self.device)
        H[diag, diag] += damp
        H = torch.linalg.cholesky(H)
        H = torch.cholesky_inverse(H)
        H = torch.linalg.cholesky(H, upper=True)
        Hinv = H

        scale = []
        zero = []

        for i1 in range(0, self.columns, blocksize):
            i2 = min(i1 + blocksize, self.columns)
            count = i2 - i1

            W1 = W[:, i1:i2].clone()
            Q1 = torch.zeros_like(W1)
            Err1 = torch.zeros_like(W1)
            Losses1 = torch.zeros_like(W1)
            Hinv1 = Hinv[i1:i2, i1:i2]

            for i in range(count):  # within a block, channel wise
                w = W1[:, i]
                d = Hinv1[i, i]

                if groupsize != -1:
                    if (i1 + i) % groupsize == 0:
                        self.quantizer.find_params(
                            W[:, (i1 + i) : (i1 + i + groupsize)], weight=True
                        )
                        scale.append(self.quantizer.scale)
                        zero.append(self.quantizer.zero)

                q = quantize(
                    w.unsqueeze(1),
                    self.quantizer.scale,
                    self.quantizer.zero,
                    self.quantizer.maxq,
                ).flatten()
                Q1[:, i] = q
                Losses1[:, i] = (w - q) ** 2 / d**2

                err1 = (w - q) / d
                W1[:, i:] -= err1.unsqueeze(1).matmul(Hinv1[i, i:].unsqueeze(0))
                Err1[:, i] = err1

            Q[:, i1:i2] = Q1
            Losses[:, i1:i2] = Losses1 / 2

            W[:, i2:] -= Err1.matmul(Hinv[i1:i2, i2:])

        # logger.info(f"time {(time.time() - tick)}")
        # logger.info(f"error {torch.sum(Losses).item()}")

        if act_order:
            invperm = torch.argsort(perm)
            Q = Q[:, invperm]

        if isinstance(self.layer, transformers.Conv1D):
            Q = Q.t()
        # self.layer.weight.data = Q.reshape(self.layer.weight.shape).to(self.layer.weight.data.dtype)
        Q = Q.reshape(weight_shape).to(weight_dtype)
        if DEBUG:
            logger.info(f"{torch.sum((self.layer(self.inp1) - self.out1) ** 2)}")

        if scale == []:
            scale.append(self.quantizer.scale)
            zero.append(self.quantizer.zero)
        scale = torch.cat(scale, dim=1)
        zero = torch.cat(zero, dim=1)
        return scale, zero, Q

    def free(self):
        if DEBUG:
            self.inp1 = None
            self.out1 = None
        self.H = None
        self.Losses = None
        self.Trace = None
        torch.cuda.empty_cache()


class Quantizer(nn.Module):
    def __init__(self, shape=1):
        super(Quantizer, self).__init__()
        self.register_buffer("maxq", torch.tensor(0))
        self.register_buffer("scale", torch.zeros(shape))
        self.register_buffer("zero", torch.zeros(shape))

    def configure(
        self,
        bits,
        perchannel=False,
        sym=True,
        mse=False,
        norm=2.4,
        grid=100,
        maxshrink=0.8,
        trits=False,
    ):
        self.maxq = torch.tensor(2**bits - 1)
        self.perchannel = perchannel
        self.sym = sym
        self.mse = mse
        self.norm = norm
        self.grid = grid
        self.maxshrink = maxshrink
        if trits:
            self.maxq = torch.tensor(-1)

    def find_params(self, x, weight=False):
        dev = x.device
        self.maxq = self.maxq.to(dev)

        shape = x.shape
        if self.perchannel:
            if weight:
                x = x.flatten(1)
            else:
                if len(shape) == 4:
                    x = x.permute([1, 0, 2, 3])
                    x = x.flatten(1)
                if len(shape) == 3:
                    x = x.reshape((-1, shape[-1])).t()
                if len(shape) == 2:
                    x = x.t()
        else:
            x = x.flatten().unsqueeze(0)

        tmp = torch.zeros(x.shape[0], device=dev)
        xmin = torch.minimum(x.min(1)[0], tmp)
        xmax = torch.maximum(x.max(1)[0], tmp)

        if self.sym:
            xmax = torch.maximum(torch.abs(xmin), xmax)
            tmp = xmin < 0
            if torch.any(tmp):
                xmin[tmp] = -xmax[tmp]
        tmp = (xmin == 0) & (xmax == 0)
        xmin[tmp] = -1
        xmax[tmp] = +1

        if self.maxq < 0:
            self.scale = xmax
            self.zero = xmin
        else:
            self.scale = (xmax - xmin) / self.maxq
            if self.sym:
                self.zero = torch.full_like(self.scale, (self.maxq + 1) / 2)
            else:
                self.zero = torch.round(-xmin / self.scale)

        if self.mse:
            best = torch.full([x.shape[0]], float("inf"), device=dev)
            for i in range(int(self.maxshrink * self.grid)):
                p = 1 - i / self.grid
                xmin1 = p * xmin
                xmax1 = p * xmax
                scale1 = (xmax1 - xmin1) / self.maxq
                zero1 = torch.round(-xmin1 / scale1) if not self.sym else self.zero
                q = quantize(x, scale1.unsqueeze(1), zero1.unsqueeze(1), self.maxq)
                q -= x
                q.abs_()
                q.pow_(self.norm)
                err = torch.sum(q, 1)
                tmp = err < best
                if torch.any(tmp):
                    best[tmp] = err[tmp]
                    self.scale[tmp] = scale1[tmp]
                    self.zero[tmp] = zero1[tmp]
        if not self.perchannel:
            if weight:
                tmp = shape[0]
            else:
                tmp = shape[1] if len(shape) != 3 else shape[2]
            self.scale = self.scale.repeat(tmp)
            self.zero = self.zero.repeat(tmp)

        if weight:
            shape = [-1] + [1] * (len(shape) - 1)
            self.scale = self.scale.reshape(shape)
            self.zero = self.zero.reshape(shape)
            return
        if len(shape) == 4:
            self.scale = self.scale.reshape((1, -1, 1, 1))
            self.zero = self.zero.reshape((1, -1, 1, 1))
        if len(shape) == 3:
            self.scale = self.scale.reshape((1, 1, -1))
            self.zero = self.zero.reshape((1, 1, -1))
        if len(shape) == 2:
            self.scale = self.scale.unsqueeze(0)
            self.zero = self.zero.unsqueeze(0)

    def ready(self):
        return torch.all(self.scale != 0)
