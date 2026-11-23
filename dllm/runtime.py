"""Checked LLaDA-MoE generation and cooperative controls.

Sampler follows the publisher's temperature-zero, low-confidence reference:
https://huggingface.co/inclusionAI/LLaDA-MoE-7B-A1B-Instruct
One full canvas block, no classifier-free guidance, no KV cache.
Imports remain lightweight so protocol/schedule tests need no ML installation.
"""
from __future__ import annotations

import contextlib
from dataclasses import asdict, dataclass
import importlib.metadata
import platform
import re
import threading
import time
from typing import Any

MODEL_ID = "inclusionAI/LLaDA-MoE-7B-A1B-Instruct"
MODEL_REVISION = "783d3467f108d28ac0a78d3e41af16ab05cabd8d"
MASK_ID = 156895
VALIDATION_PROMPT = (
    "You control a fighter. Health 60, enemy health 40, distance 3 meters. "
    "Available actions: dodge, attack, retreat. Choose an action and explain briefly."
)


def transfer_schedule(tokens: int, steps: int) -> list[int]:
    """Publisher quotient/remainder schedule; reject redundant zero-transfer work."""
    if type(tokens) is not int or type(steps) is not int or not 1 <= steps <= tokens <= 2048:
        raise ValueError("Require integer 1 <= steps <= output tokens <= 2048")
    base, remainder = divmod(tokens, steps)
    return [base + (i < remainder) for i in range(steps)]


class GenerationCancelled(RuntimeError):
    pass


def validate_tokenizer(tokenizer):
    # [gMASK] is a separate, adjacent vocabulary entry (156894). The publisher
    # uses <|mask|> (156895), also exposed by this revision's mask_token_id.
    if (tokenizer.mask_token_id != MASK_ID or
        tokenizer.convert_ids_to_tokens(MASK_ID) != "<|mask|>" or
        tokenizer.convert_tokens_to_ids("<|mask|>") != MASK_ID):
        raise ValueError("Pinned LLaDA-MoE tokenizer must map <|mask|> to 156895")


class CooperativeGate:
    """Acknowledgement occurs only at a synchronized worker boundary.

    Manual controls do not implement game admission. Model access remains on one
    owner thread; control requests only mutate this condition-protected state.
    """
    def __init__(self):
        self.condition = threading.Condition()
        self.pause_requested = False
        self.paused = False
        self.stopped = False
        self.running = False
        self.completed_chunks = 0

    def begin(self):
        with self.condition:
            if self.running:
                raise RuntimeError("Generation already running")
            self.running = True
            self.stopped = False
            self.completed_chunks = 0
            self.condition.notify_all()

    def finish(self):
        with self.condition:
            self.running = False
            self.paused = False
            self.condition.notify_all()

    def checkpoint(self):
        # Caller must synchronize all its CUDA work BEFORE entering this gate.
        start = time.perf_counter_ns()
        with self.condition:
            while self.pause_requested and not self.stopped:
                self.paused = True
                self.condition.notify_all()
                self.condition.wait()
            self.paused = False
            if self.stopped:
                raise GenerationCancelled("Stopped at a synchronized boundary")
        return (time.perf_counter_ns() - start) / 1e6

    def completed(self):
        with self.condition:
            self.completed_chunks += 1
            self.condition.notify_all()

    def pause(self):
        with self.condition:
            self.pause_requested = True
            self.condition.notify_all()

    def resume(self):
        with self.condition:
            self.pause_requested = False
            self.condition.notify_all()

    def stop(self):
        with self.condition:
            self.stopped = True
            self.pause_requested = False
            self.condition.notify_all()

    def wait_paused(self, timeout: float) -> bool:
        with self.condition:
            return self.condition.wait_for(lambda: self.paused or not self.running,
                                           timeout) and self.paused

    def status(self) -> dict:
        with self.condition:
            return {key: getattr(self, key) for key in (
                "running", "pause_requested", "paused", "stopped", "completed_chunks")}


@dataclass
class GenerationResult:
    content: str
    output_ids: list[int]
    canvas_ids: list[int]
    prompt_tokens: int
    requested_tokens: int
    steps_requested: int
    step_wall_ms: list[float]
    transfer_counts: list[int]
    generation_wall_ms: float
    tokenization_ms: float
    admission_wait_ms: float
    unresolved_masks: int
    stop_reason: str

    def to_dict(self):
        value = asdict(self)
        value["generated_tokens"] = len(self.output_ids)
        value["executed_forwards"] = len(self.step_wall_ms)
        value["timing_source"] = "diffusion-host-wall-v1"
        value["action"] = parse_action(self.content)
        return value


def parse_action(content: str) -> str | None:
    match = re.match(r"\s*(dodge|attack|retreat)\b", content, re.IGNORECASE)
    return match.group(1).lower() if match else None


def environment_manifest() -> dict:
    result = {"python": platform.python_version(), "platform": platform.platform()}
    for name in ("torch", "transformers", "accelerate", "bitsandbytes", "numpy"):
        try:
            result[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            result[name] = None
    return result


class LladaRuntime:
    def __init__(self, model, tokenizer, device: str, metadata: dict):
        self.model, self.tokenizer, self.device = model, tokenizer, device
        self.metadata = metadata
        self.gate = CooperativeGate()
        self.owner = threading.Lock()
        validate_tokenizer(tokenizer)

    def synchronize(self):
        import torch
        if self.device.startswith("cuda"):
            torch.cuda.synchronize(self.device)

    @contextlib.contextmanager
    def skip_layers(self, count: int):
        import torch
        layers = getattr(getattr(self.model, "model", None), "layers", None)
        if type(count) is not int or count < 0:
            raise ValueError("skip_layers must be a nonnegative integer")
        if count == 0:
            yield []
            return
        if not isinstance(layers, torch.nn.ModuleList) or count > len(layers) - 2:
            raise ValueError("Unsupported layer layout or skip count; preserve first and final blocks")
        skipped = list(range(len(layers) - 1 - count, len(layers) - 1))
        original_count = self.model.config.num_hidden_layers
        try:
            self.model.model.layers = torch.nn.ModuleList(
                [layer for i, layer in enumerate(layers) if i not in skipped])
            self.model.config.num_hidden_layers = len(self.model.model.layers)
            yield skipped
        finally:
            self.model.model.layers = layers
            self.model.config.num_hidden_layers = original_count

    def generate(self, prompt: str, *, tokens=16, steps=16, skip_layers=0,
                 temperature=0.0, seed=1, raw_prompt=False, on_step=None) -> GenerationResult:
        import torch
        import torch.nn.functional as F
        schedule = transfer_schedule(tokens, steps)
        if not isinstance(prompt, str) or not prompt.strip():
            raise ValueError("prompt must be nonempty text")
        if temperature != 0 or type(seed) is not int:
            raise ValueError("Validated sampler supports temperature=0 and integer seed only")
        if not self.owner.acquire(blocking=False):
            raise RuntimeError("Only one model request may run at a time")
        try:
            self.gate.begin()
            tokenize_start = time.perf_counter_ns()
            formatted = prompt if raw_prompt else self.tokenizer.apply_chat_template(
                [{"role": "system", "content": "You are a helpful AI assistant."},
                 {"role": "user", "content": prompt}],
                add_generation_prompt=True, tokenize=False)
            encoded = self.tokenizer(formatted, return_tensors="pt", add_special_tokens=False)
            input_ids = encoded.input_ids.to(self.device)
            prompt_len = input_ids.shape[1]
            if prompt_len + tokens > self.model.config.max_position_embeddings:
                raise ValueError("Prompt plus output exceeds model context")
            if (input_ids == MASK_ID).any().item():
                raise ValueError("Prompt must not contain the diffusion mask token")
            self.synchronize()
            tokenization_ms = (time.perf_counter_ns() - tokenize_start) / 1e6
            start = time.perf_counter_ns()
            step_times, transfers = [], []
            wait_ms = 0.0
            with self.skip_layers(skip_layers), torch.inference_mode():
                x = torch.full((1, prompt_len + tokens), MASK_ID,
                               dtype=torch.long, device=self.device)
                x[:, :prompt_len] = input_ids
                self.synchronize()
                for step, k in enumerate(schedule):
                    wait_ms += self.gate.checkpoint()
                    step_start = time.perf_counter_ns()
                    mask_index = x == MASK_ID
                    outputs = self.model(input_ids=x, use_cache=False, return_dict=True)
                    logits = getattr(outputs, "logits", None)
                    if logits is None or logits.ndim != 3 or logits.shape[:2] != x.shape:
                        raise ValueError("Model did not return canvas-aligned vocabulary logits")
                    predictions = torch.argmax(logits, dim=-1)
                    probabilities = F.softmax(logits, dim=-1)
                    confidence = torch.gather(probabilities, -1,
                                              predictions.unsqueeze(-1)).squeeze(-1)
                    predictions = torch.where(mask_index, predictions, x)
                    confidence = torch.where(mask_index, confidence, -torch.inf)
                    indices = torch.topk(confidence[0], k=k).indices
                    x[0, indices] = predictions[0, indices]
                    self.synchronize()
                    step_times.append((time.perf_counter_ns() - step_start) / 1e6)
                    transfers.append(k)
                    self.gate.completed()
                    if on_step:
                        on_step(step, x[:, prompt_len:].detach().cpu().tolist()[0])
                    # Do not retain logits/softmax from the previous step across
                    # admission waits or while allocating the next forward.
                    del outputs, logits, probabilities, confidence, predictions, mask_index, indices
                # A pause requested during the final step is acknowledged too.
                wait_ms += self.gate.checkpoint()
                canvas = x[0, prompt_len:].cpu().tolist()
                unresolved = canvas.count(MASK_ID)
                if unresolved:
                    raise ValueError("Generation ended with unresolved diffusion masks")
                stop_ids = {self.tokenizer.eos_token_id}
                role_end = self.tokenizer.convert_tokens_to_ids("<|role_end|>")
                if role_end is not None and role_end != self.tokenizer.unk_token_id:
                    stop_ids.add(role_end)
                end = next((i for i, token in enumerate(canvas) if token in stop_ids), len(canvas))
                special = set(self.tokenizer.all_special_ids)
                visible = [token for token in canvas[:end] if token not in special]
                content = self.tokenizer.decode(visible, skip_special_tokens=False).strip()
                self.synchronize()
                return GenerationResult(content, visible, canvas, prompt_len, tokens, steps,
                                        step_times, transfers,
                                        (time.perf_counter_ns() - start) / 1e6,
                                        tokenization_ms, wait_ms, unresolved,
                                        "eos" if end < len(canvas) else "length")
        finally:
            self.gate.finish()
            self.owner.release()


def load_runtime(*, model_id=MODEL_ID, revision=MODEL_REVISION, device="cuda:0",
                 four_bit=True, cache_dir=None, local_files_only=False) -> LladaRuntime:
    """Load only the reviewed architecture; no silent precision/offload fallback."""
    import torch
    from transformers import AutoModel, AutoTokenizer, BitsAndBytesConfig
    if model_id != MODEL_ID or revision != MODEL_REVISION:
        raise ValueError("Use the reviewed LLaDA-MoE model and pinned immutable revision")
    if not device.startswith("cuda") or not torch.cuda.is_available():
        raise ValueError("Target validation requires an available CUDA device")
    index = torch.device(device).index
    index = torch.cuda.current_device() if index is None else index
    device = f"cuda:{index}"
    # is_available() may use NVML without initializing a CUDA context. Resetting
    # allocator statistics before lazy initialization fails in a fresh process.
    torch.cuda.init()
    torch.cuda.set_device(index)
    shared = dict(revision=revision, trust_remote_code=True, cache_dir=cache_dir,
                  local_files_only=local_files_only)
    tokenizer = AutoTokenizer.from_pretrained(model_id, **shared)
    validate_tokenizer(tokenizer)
    kwargs: dict[str, Any] = dict(torch_dtype=torch.bfloat16,
                                  device_map={"": index}, attn_implementation="sdpa")
    if four_bit:
        kwargs["quantization_config"] = BitsAndBytesConfig(
            load_in_4bit=True, bnb_4bit_compute_dtype=torch.bfloat16,
            bnb_4bit_quant_type="nf4", bnb_4bit_use_double_quant=False)
    torch.cuda.reset_peak_memory_stats(device)
    model, loading = AutoModel.from_pretrained(model_id, **shared, **kwargs, output_loading_info=True)
    model.eval()
    if any(loading.get(key) for key in ("missing_keys", "unexpected_keys", "mismatched_keys", "error_msgs")):
        raise RuntimeError(f"Checkpoint loading was incomplete: {loading}")
    torch.cuda.synchronize(device)
    actual_four_bit = bool(getattr(model, "is_loaded_in_4bit", False))
    if actual_four_bit != four_bit:
        raise RuntimeError("Loaded quantization does not match requested precision")
    placements = sorted({str(p.device) for p in model.parameters()})
    if placements != [device]:
        raise RuntimeError(f"Unexpected model placement: {placements}; require one GPU")
    load_peak = torch.cuda.max_memory_allocated(device)
    load_reserved = torch.cuda.memory_reserved(device)
    # Quantization's temporary buffers need not occupy the game's memory budget
    # throughout measurement. Live tensors are preserved; record both states.
    torch.cuda.empty_cache()
    metadata = {"model_id": model_id, "model_revision": revision,
                "tokenizer_revision": revision, "remote_code_revision": revision,
                "device": device, "device_name": torch.cuda.get_device_name(device),
                "compute_capability": list(torch.cuda.get_device_capability(device)),
                "quantization": "nf4-bf16" if actual_four_bit else "bf16",
                "four_bit_actual": actual_four_bit, "placements": placements,
                "checkpoint_loading_complete": True,
                "quantized_linear_count": sum(type(m).__name__ == "Linear4bit" for m in model.modules()),
                "load_peak_allocated_bytes": load_peak,
                "reserved_before_load_cache_release_bytes": load_reserved,
                "load_cache_released": True,
                "temperature": 0, "seed": 1, "prompt_format": "pinned-instruct-chat-template",
                "deterministic_algorithms": torch.are_deterministic_algorithms_enabled(),
                "matmul_allow_tf32": torch.backends.cuda.matmul.allow_tf32,
                "attention": "sdpa-full-noncausal", "mask_id": MASK_ID, "mask_token": "<|mask|>",
                "loaded_allocated_bytes": torch.cuda.memory_allocated(device),
                "loaded_reserved_bytes": torch.cuda.memory_reserved(device),
                "cuda_runtime": torch.version.cuda, "environment": environment_manifest(),
                "sampler": "llada-reference-low-confidence-single-block-greedy-v1",
                "scheduling": "manual-cooperative-step-boundaries"}
    return LladaRuntime(model, tokenizer, device, metadata)
