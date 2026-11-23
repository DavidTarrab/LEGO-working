"""Actual CUDA/4-bit smoke; no model weights needed."""
import argparse
import json
from pathlib import Path

from runtime import environment_manifest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError("Do not overwrite environment evidence")
    import torch
    import bitsandbytes as bnb
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    torch.cuda.init()
    torch.cuda.set_device(0)
    torch.cuda.reset_peak_memory_stats(0)
    torch.manual_seed(1)
    layer = bnb.nn.Linear4bit(128, 128, bias=False, compute_dtype=torch.bfloat16,
                             quant_type="nf4").to("cuda:0")
    x = torch.randn(1, 128, device="cuda:0", dtype=torch.bfloat16)
    y = layer(x)
    torch.cuda.synchronize()
    if not torch.isfinite(y).all().item():
        raise RuntimeError("NF4 kernel produced non-finite output")
    data = {"success": True, "kind": "environment-smoke-not-model-performance",
            "environment": environment_manifest(), "cuda": torch.version.cuda,
            "gpu": torch.cuda.get_device_name(0),
            "compute_capability": list(torch.cuda.get_device_capability(0)),
            "nf4_finite": True, "nf4_output_shape": list(y.shape),
            "cuda_architectures": torch.cuda.get_arch_list()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(data), flush=True)


if __name__ == "__main__":
    main()
