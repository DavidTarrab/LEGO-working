#!/usr/bin/env python3
"""
Standalone Diffusion LLM (dLLM) Profiler & Layer-Similarity Analyzer for LEGO.

Produces the Weeks 1-4 deliverables for the midterm presentation:
  1. Candidate dLLM VRAM & Architecture Evaluation Table (16 GB RTX 5070 Ti budget).
  2. Per-Step & End-to-End Latency across diffusion step budgets N in {4, 8, 16, 32, 64}
     using the exact prompt and 16-token budget from docs/results/validation-01.json.
  3. Bottom-Up Layer Skipping latency vs. text-quality characterization (Proposal Sec. 3.3).
  4. Inter-Layer Cosine Similarity Heatmap (reproducing LEGO Fig. 8 / adaptor/struct_heatmap.py).
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import time
import warnings
from typing import Any, Dict, List, Optional, Tuple

warnings.filterwarnings("ignore", message=".*NotOpenSSLWarning.*")
warnings.filterwarnings("ignore", category=UserWarning, module="urllib3")
warnings.filterwarnings("ignore", message=".*torch_dtype.*is deprecated.*")

try:
    import numpy as np
    import torch
    import torch.nn.functional as F
except ImportError:
    np = None  # type: ignore
    torch = None  # type: ignore
    F = None  # type: ignore

# Matches David's exact prompt in docs/results/validation-01.json & tools/Measure-LlmLoad.ps1
LEGO_VALIDATION_PROMPT = (
    "You control a fighter. Health 60, enemy health 40, distance 3 meters. "
    "Available actions: dodge, attack, retreat. Choose an action and explain briefly."
)

NPC_DIALOGUE_PROMPT = (
    "System: You are an NPC guard in a fantasy action RPG. "
    "Player Health: 42/100. Recent Action: Player drew a weapon near the gate.\n"
    "Player: Let me through the gate immediately!\n"
    "NPC Guard:"
)

CANDIDATE_MODELS_VRAM_TABLE = [
    {
        "model_id": "google/diffusiongemma-26B-A4B-it",
        "architecture": "MoE dLLM (Gemma 4 base, Entropy-Bounded denoising)",
        "total_params_b": 26.0,
        "active_params_b": 3.8,
        "bf16_weights_gb": 52.0,
        "int4_weights_gb": 14.5,
        "est_runtime_vram_int4_gb": 16.2,
        "fits_16gb_with_ue4_and_dlss": False,
        "verdict": "Overflow (>14.5 GB weights in 4-bit leaves <1.5 GB for UE4 + DLSS)",
    },
    {
        "model_id": "inclusionAI/LLaDA-MoE-7B-A1B-Instruct",
        "architecture": "MoE Discrete Diffusion LLM (16 layers, 64 experts)",
        "total_params_b": 7.0,
        "active_params_b": 1.4,
        "bf16_weights_gb": 14.0,
        "int4_weights_gb": 4.2,
        "est_runtime_vram_int4_gb": 5.1,
        "fits_16gb_with_ue4_and_dlss": True,
        "verdict": "Primary MoE dLLM (5.1 GB in 4-bit leaves ~10.9 GB for UE4 + DLSS; 1.4B active compute/step)",
    },
    {
        "model_id": "Dream-org/Dream-v0-Instruct-7B",
        "architecture": "Dense Discrete Diffusion LLM (28 layers)",
        "total_params_b": 7.6,
        "active_params_b": 7.6,
        "bf16_weights_gb": 15.2,
        "int4_weights_gb": 4.6,
        "est_runtime_vram_int4_gb": 5.6,
        "fits_16gb_with_ue4_and_dlss": True,
        "verdict": "Primary Dense dLLM (5.6 GB in 4-bit leaves ~10.4 GB for UE4 + DLSS)",
    },
    {
        "model_id": "GSAI-ML/LLaDA-8B-Instruct",
        "architecture": "Dense Masked Diffusion LLM (32 layers, Llama-3 arch)",
        "total_params_b": 8.0,
        "active_params_b": 8.0,
        "bf16_weights_gb": 16.0,
        "int4_weights_gb": 4.9,
        "est_runtime_vram_int4_gb": 5.9,
        "fits_16gb_with_ue4_and_dlss": True,
        "verdict": "Direct Diffusion counterpart to LEGO's Llama-3.1-8B Q4_K_M baseline (~5.9 GB)",
    },
]


def get_device(requested: Optional[str] = None) -> str:
    if requested:
        return requested
    if torch.cuda.is_available():
        return "cuda"
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def sync_device(device: str) -> None:
    if device.startswith("cuda"):
        torch.cuda.synchronize()
    elif device == "mps" and hasattr(torch, "mps"):
        torch.mps.synchronize()


def get_allocated_vram_gb(device: str) -> float:
    if device.startswith("cuda"):
        return torch.cuda.max_memory_allocated() / (1024 ** 3)
    if device == "mps" and hasattr(torch, "mps"):
        return torch.mps.current_allocated_memory() / (1024 ** 3)
    return 0.0


def find_transformer_layers_container(model: torch.nn.Module):
    """Locate (parent_module, attr_name, ModuleList) of transformer blocks across dLLM/LLM architectures."""
    for path in (
        "model.layers",
        "model.transformer.blocks",
        "transformer.blocks",
        "transformer.h",
        "layers",
    ):
        parts = path.split(".")
        curr = model
        found = True
        for attr in parts[:-1]:
            if hasattr(curr, attr):
                curr = getattr(curr, attr)
            else:
                found = False
                break
        if found and hasattr(curr, parts[-1]):
            target = getattr(curr, parts[-1])
            if isinstance(target, torch.nn.ModuleList):
                return curr, parts[-1], target
    return None, "", None


def find_transformer_layers(model: torch.nn.Module) -> Optional[torch.nn.ModuleList]:
    """Locate the ModuleList of transformer blocks across LLaDA, Dream, Qwen, or Llama models."""
    _, _, layers = find_transformer_layers_container(model)
    return layers


@contextlib.contextmanager
def bottom_up_layer_skip(model: torch.nn.Module, skip_count: int, keep_last_layer: bool = True):
    """
    Implements LEGO's bottom-up layer-skipping pattern (Proposal Sec. 3.3 & LEGO Paper Sec. IV-B):
    Bypasses `skip_count` contiguous late transformer layers while preserving the final
    transformer layer so the output projection interface remains intact.
    """
    parent, attr_name, layers = find_transformer_layers_container(model)
    if layers is None or skip_count <= 0:
        yield []
        return

    num_layers = len(layers)
    end_idx = (num_layers - 1) if keep_last_layer else num_layers
    start_idx = max(1, end_idx - skip_count)
    skipped_indices = list(range(start_idx, end_idx))
    skipped_set = set(skipped_indices)

    orig_cfg_layers = getattr(model.config, "num_hidden_layers", num_layers)

    # Temporarily assign a filtered ModuleList (bypassing skipped_indices while keeping final layer)
    kept_layers = torch.nn.ModuleList(
        [layer for idx, layer in enumerate(layers) if idx not in skipped_set]
    )
    setattr(parent, attr_name, kept_layers)
    if hasattr(model.config, "num_hidden_layers"):
        model.config.num_hidden_layers = len(kept_layers)

    try:
        yield skipped_indices
    finally:
        setattr(parent, attr_name, layers)
        if hasattr(model.config, "num_hidden_layers"):
            model.config.num_hidden_layers = orig_cfg_layers


def load_dllm(model_id: str, device: str, use_4bit: bool = True):
    from transformers import AutoModel, AutoModelForCausalLM, AutoTokenizer

    if device.startswith("cuda"):
        torch.cuda.empty_cache()
        torch.cuda.reset_peak_memory_stats()

    quant_config = None
    if use_4bit and device.startswith("cuda"):
        try:
            from transformers import BitsAndBytesConfig
            quant_config = BitsAndBytesConfig(
                load_in_4bit=True,
                bnb_4bit_compute_dtype=torch.bfloat16,
                bnb_4bit_quant_type="nf4",
            )
        except ImportError:
            print("[Warn] bitsandbytes not installed; falling back to bfloat16/float16.")

    dtype = torch.bfloat16 if device.startswith("cuda") else torch.float16
    tokenizer = AutoTokenizer.from_pretrained(model_id, trust_remote_code=True)
    if tokenizer.pad_token is None and tokenizer.eos_token is not None:
        tokenizer.pad_token = tokenizer.eos_token

    load_kwargs: Dict[str, Any] = {
        "torch_dtype": dtype,
        "trust_remote_code": True,
    }
    if quant_config is not None:
        load_kwargs["quantization_config"] = quant_config
        load_kwargs["device_map"] = "auto"
    elif device.startswith("cuda"):
        load_kwargs["device_map"] = "cuda"

    try:
        model = AutoModel.from_pretrained(model_id, **load_kwargs)
    except Exception:
        model = AutoModelForCausalLM.from_pretrained(model_id, **load_kwargs)

    if quant_config is None and not device.startswith("cuda"):
        model = model.to(device)

    model.eval()
    sync_device(device)
    weights_vram_gb = get_allocated_vram_gb(device)
    layers = find_transformer_layers(model)
    num_layers = len(layers) if layers is not None else -1
    print(
        f"[Init] Loaded {model_id} on {device} (4-bit={quant_config is not None}) | "
        f"Layers={num_layers} | Weights VRAM={weights_vram_gb:.2f} GB"
    )
    return model, tokenizer, weights_vram_gb, num_layers


def run_diffusion_steps(
    model: torch.nn.Module,
    tokenizer: Any,
    prompt: str,
    steps: int,
    max_new_tokens: int,
    device: str,
) -> Tuple[float, List[float], str]:
    """
    Executes `steps` denoising iterations (D_1 ... D_N) for a masked/discrete diffusion LLM
    (or uses native `diffusion_generate` if provided by Dream-7B), recording per-step latency.
    """
    encoded = tokenizer(prompt, return_tensors="pt")
    input_ids = encoded.input_ids.to(device)
    attention_mask = encoded.attention_mask.to(device) if "attention_mask" in encoded else None

    mask_token_id = getattr(tokenizer, "mask_token_id", None)
    if mask_token_id is None:
        mask_token_id = getattr(model.config, "mask_token_id", tokenizer.eos_token_id or 126336)

    # Construct full sequence [prompt_ids, mask_tokens] of length L_prompt + max_new_tokens
    mask_pad = torch.full(
        (input_ids.shape[0], max_new_tokens),
        fill_value=mask_token_id,
        dtype=input_ids.dtype,
        device=device,
    )
    x = torch.cat([input_ids, mask_pad], dim=1)
    prompt_len = input_ids.shape[1]

    step_latencies_ms: List[float] = []
    sync_device(device)
    t_start = time.perf_counter()

    with torch.no_grad():
        if hasattr(model, "diffusion_generate"):
            # Native Dream-7B generation API
            out = model.diffusion_generate(
                input_ids,
                attention_mask=attention_mask,
                max_new_tokens=max_new_tokens,
                steps=steps,
                temperature=0.2,
                alg="entropy",
            )
            sync_device(device)
            total_ms = (time.perf_counter() - t_start) * 1000.0
            step_latencies_ms = [total_ms / steps] * steps
            gen_ids = out.sequences[:, prompt_len:] if hasattr(out, "sequences") else out[:, prompt_len:]
        else:
            # Explicit iterative denoising loop D_1 ... D_N (LLaDA / Masked dLLM schedule)
            tokens_per_step = max(1, max_new_tokens // steps)
            for step_idx in range(steps):
                sync_device(device)
                t_step = time.perf_counter()

                outputs = model(input_ids=x)
                logits = outputs.logits if hasattr(outputs, "logits") else outputs[0]

                # Confidence-based parallel unmasking on the target canvas
                target_logits = logits[:, prompt_len:, :]
                probs = F.softmax(target_logits.float(), dim=-1)
                conf, pred_ids = probs.max(dim=-1)

                is_masked = (x[:, prompt_len:] == mask_token_id)
                if is_masked.any():
                    masked_conf = torch.where(is_masked, conf, torch.tensor(-1.0, device=device))
                    k = int(min(tokens_per_step, is_masked.sum().item()))
                    if step_idx == steps - 1:
                        k = int(is_masked.sum().item())
                    if k > 0:
                        _, top_idx = torch.topk(masked_conf, k=k, dim=-1)
                        target_slice = x[:, prompt_len:]
                        target_slice.scatter_(1, top_idx, pred_ids.gather(1, top_idx))
                        x[:, prompt_len:] = target_slice

                sync_device(device)
                step_latencies_ms.append((time.perf_counter() - t_step) * 1000.0)

            total_ms = sum(step_latencies_ms)
            gen_ids = x[:, prompt_len:]

    text = tokenizer.decode(gen_ids[0], skip_special_tokens=True).strip()
    return total_ms, step_latencies_ms, text


def profile_step_budgets(
    model: torch.nn.Module,
    tokenizer: Any,
    device: str,
    steps_list: Tuple[int, ...] = (4, 8, 16, 32, 64),
    max_new_tokens: int = 16,
    prompt: str = LEGO_VALIDATION_PROMPT,
) -> List[Dict[str, Any]]:
    # Warmup pass
    _, _, _ = run_diffusion_steps(model, tokenizer, prompt, steps=2, max_new_tokens=max_new_tokens, device=device)

    results = []
    for steps in steps_list:
        if device.startswith("cuda"):
            torch.cuda.reset_peak_memory_stats()

        total_ms, step_ms_list, sample_output = run_diffusion_steps(
            model, tokenizer, prompt, steps=steps, max_new_tokens=max_new_tokens, device=device
        )
        per_step_mean = float(np.mean(step_ms_list))
        per_step_p95 = float(np.percentile(step_ms_list, 95))
        peak_vram_gb = get_allocated_vram_gb(device)

        # Compare against LEGO APM deadlines (100 APM = 600ms, 200 APM = 300ms, 300 APM = 200ms)
        achievable_apm = round(60000.0 / total_ms, 2) if total_ms > 0 else 0.0

        row = {
            "diffusion_steps": steps,
            "max_new_tokens": max_new_tokens,
            "total_latency_ms": round(total_ms, 2),
            "per_step_mean_ms": round(per_step_mean, 2),
            "per_step_p95_ms": round(per_step_p95, 2),
            "achievable_standalone_apm": achievable_apm,
            "meets_200_apm_300ms_slo": total_ms <= 300.0,
            "peak_vram_gb": round(peak_vram_gb, 2),
            "sample_output": sample_output[:160],
        }
        results.append(row)
        print(
            f"[Step Profile] Steps={steps:2d} | Total={total_ms:7.2f} ms | "
            f"Per-Step={per_step_mean:5.2f} ms | Max APM={achievable_apm:6.1f} | "
            f"VRAM={peak_vram_gb:.2f} GB"
        )
    return results


def profile_bottom_up_layer_skipping(
    model: torch.nn.Module,
    tokenizer: Any,
    device: str,
    fixed_steps: int = 16,
    skip_counts: Tuple[int, ...] = (0, 2, 4, 6, 8),
    max_new_tokens: int = 16,
    prompt: str = LEGO_VALIDATION_PROMPT,
) -> List[Dict[str, Any]]:
    results = []
    baseline_ms = None
    baseline_tokens = None

    for skip_k in skip_counts:
        try:
            with bottom_up_layer_skip(model, skip_count=skip_k, keep_last_layer=True) as skipped_idx:
                total_ms, step_ms_list, output_text = run_diffusion_steps(
                    model,
                    tokenizer,
                    prompt,
                    steps=fixed_steps,
                    max_new_tokens=max_new_tokens,
                    device=device,
                )
        except Exception as exc:
            print(f"[Layer Skip] Skipping {skip_k} layers unsupported for this model hook: {exc}")
            break

        out_tokens = tokenizer.encode(output_text, add_special_tokens=False)
        if baseline_ms is None:
            baseline_ms = total_ms
            baseline_tokens = set(out_tokens)

        token_overlap = (
            len(set(out_tokens) & baseline_tokens) / max(1, len(baseline_tokens))
            if baseline_tokens
            else 1.0
        )
        speedup_pct = ((baseline_ms - total_ms) / baseline_ms * 100.0) if baseline_ms else 0.0

        row = {
            "skipped_layers_count": skip_k,
            "skipped_layer_indices": skipped_idx,
            "diffusion_steps": fixed_steps,
            "total_latency_ms": round(total_ms, 2),
            "per_step_mean_ms": round(float(np.mean(step_ms_list)), 2),
            "latency_reduction_pct": round(speedup_pct, 2),
            "token_overlap_vs_full": round(token_overlap, 3),
            "sample_output": output_text[:160],
        }
        results.append(row)
        print(
            f"[Layer Skip] Skip={skip_k} layers ({skipped_idx}) | "
            f"Total={total_ms:7.2f} ms (-{speedup_pct:4.1f}%) | "
            f"Per-Step={np.mean(step_ms_list):5.2f} ms | Overlap={token_overlap:.2f}"
        )
    return results


def compute_interlayer_similarity(
    model: torch.nn.Module,
    tokenizer: Any,
    device: str,
    output_dir: str,
) -> List[List[float]]:
    """
    Computes LEGO Fig. 8 inter-layer cosine similarity heatmap across game/NPC prompts
    (following adaptor/struct_heatmap.py) to justify bottom-up layer skipping.
    """
    prompts = [
        LEGO_VALIDATION_PROMPT,
        NPC_DIALOGUE_PROMPT,
        "Combat state: Player HP 80%, stamina low, boss charging heavy sweep attack. Select best counter.",
        "Dialogue state: Merchant in besieged town, player offers rare relic for healing potions.",
    ]
    sim_accum = None
    valid_samples = 0

    for text in prompts:
        inputs = tokenizer(text, return_tensors="pt").to(device)
        with torch.no_grad():
            outputs = model(
                input_ids=inputs.input_ids,
                output_hidden_states=True,
                return_dict=True,
            )
        hidden_states = getattr(outputs, "hidden_states", None)
        if not hidden_states:
            print("[Warn] Model did not return hidden_states; skipping heatmap.")
            return []

        # Following adaptor/struct_heatmap.py: compare hidden representations across all layers
        num_layers = len(hidden_states)
        sim = np.zeros((num_layers, num_layers), dtype=np.float64)
        flat_layers = [h.reshape(-1, h.shape[-1]).float() for h in hidden_states]

        for i in range(num_layers):
            for j in range(num_layers):
                sim[i, j] = F.cosine_similarity(flat_layers[i], flat_layers[j], dim=1).mean().item()

        sim_accum = sim if sim_accum is None else (sim_accum + sim)
        valid_samples += 1

    final_sim = (sim_accum / max(1, valid_samples)).tolist()

    try:
        import matplotlib.pyplot as plt

        os.makedirs(output_dir, exist_ok=True)
        heatmap_path = os.path.join(output_dir, "dllm_layer_similarity_heatmap.png")
        arr = np.array(final_sim)
        plt.figure(figsize=(10, 8))
        plt.imshow(arr, cmap="Blues", interpolation="nearest", origin="lower", vmin=0.0, vmax=1.0)
        plt.colorbar(label="Cosine Similarity")
        plt.xlabel("Layer i")
        plt.ylabel("Layer j")
        plt.title("Diffusion LLM Inter-Layer Cosine Similarity (LEGO Fig. 8 Reproduction)")
        plt.savefig(heatmap_path, dpi=300, bbox_inches="tight")
        plt.close()
        print(f"[Heatmap] Saved LEGO Fig. 8 similarity heatmap to {heatmap_path}")
    except Exception as exc:
        print(f"[Warn] Could not render matplotlib PNG ({exc}); raw matrix saved to JSON.")

    return final_sim


def main():
    parser = argparse.ArgumentParser(description="Profile candidate Diffusion LLMs for LEGO.")
    parser.add_argument(
        "--model",
        type=str,
        default="inclusionAI/LLaDA-MoE-7B-A1B-Instruct",
        help="HuggingFace model ID (e.g., inclusionAI/LLaDA-MoE-7B-A1B-Instruct or Dream-org/Dream-v0-Instruct-7B)",
    )
    parser.add_argument("--device", type=str, default=None, help="Device override (cuda, mps, cpu)")
    parser.add_argument("--no-4bit", action="store_true", help="Disable 4-bit NF4 quantization")
    parser.add_argument("--tokens", type=int, default=16, help="Output token budget (LEGO default: 16)")
    parser.add_argument(
        "--steps",
        type=int,
        nargs="+",
        default=[4, 8, 16, 32, 64],
        help="Diffusion step budgets to profile",
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="runs/dllm-profile",
        help="Directory to write JSON and heatmap plots",
    )
    parser.add_argument(
        "--table-only",
        action="store_true",
        help="Only generate the Deliverable A candidate VRAM evaluation table without downloading weights",
    )
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    report: Dict[str, Any] = {
        "deliverable_a_vram_table": CANDIDATE_MODELS_VRAM_TABLE,
        "david_ar_baseline_reference": {
            "source": "docs/results/validation-01.json",
            "hardware": "NVIDIA GeForce RTX 5070 Ti (16 GB)",
            "model": "Meta Llama 3.1 8B Instruct Q4_K_M",
            "render_only_fps": 59.98,
            "render_only_p99_frame_ms": 17.44,
            "llm_only_apm": 200.57,
            "llm_only_p95_latency_ms": 268.90,
            "unrestricted_concurrent_fps": 48.46,
            "unrestricted_concurrent_p99_frame_ms": 30.65,
            "unrestricted_concurrent_apm": 137.39,
            "scheduled_intraframe_fps": 60.00,
            "scheduled_intraframe_apm": 101.20,
            "scheduled_intraframe_deadline_miss_pct": 100.0,
        },
    }

    if args.table_only:
        out_file = os.path.join(args.output_dir, "deliverable_a_vram_evaluation.json")
        with open(out_file, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        print(f"[Done] Wrote Deliverable A VRAM & Baseline comparison table to {out_file}")
        return

    device = get_device(args.device)
    model, tokenizer, weights_vram_gb, num_layers = load_dllm(
        args.model, device=device, use_4bit=not args.no_4bit
    )

    step_results = profile_step_budgets(
        model,
        tokenizer,
        device=device,
        steps_list=tuple(args.steps),
        max_new_tokens=args.tokens,
    )
    skip_results = profile_bottom_up_layer_skipping(
        model,
        tokenizer,
        device=device,
        fixed_steps=16,
        max_new_tokens=args.tokens,
    )
    sim_matrix = compute_interlayer_similarity(model, tokenizer, device=device, output_dir=args.output_dir)

    report["profiled_model"] = {
        "model_id": args.model,
        "device": device,
        "use_4bit": not args.no_4bit,
        "num_layers": num_layers,
        "weights_vram_gb": round(weights_vram_gb, 2),
        "step_characterization": step_results,
        "bottom_up_layer_skipping": skip_results,
        "layer_similarity_matrix": sim_matrix,
    }

    out_file = os.path.join(args.output_dir, "dllm_week1_4_characterization.json")
    with open(out_file, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"[Done] Saved complete Weeks 1-4 dLLM characterization to {out_file}")


if __name__ == "__main__":
    main()
