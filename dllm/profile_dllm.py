#!/usr/bin/env python3
"""Repeated CUDA characterization; raw samples and provenance are retained."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import time

from runtime import MODEL_ID, MODEL_REVISION, VALIDATION_PROMPT, load_runtime, transfer_schedule


def atomic_json(path: Path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def source_manifest():
    root = Path(__file__).resolve().parent.parent
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                              capture_output=True, text=True, check=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=root,
                           capture_output=True, text=True, check=True).stdout.strip()
    return {"git_revision": revision, "dirty": bool(dirty),
            "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted((root / "dllm").glob("*.py"))}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=MODEL_ID)
    parser.add_argument("--revision", default=MODEL_REVISION)
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--no-4bit", action="store_true")
    parser.add_argument("--cache-dir")
    parser.add_argument("--local-files-only", action="store_true")
    parser.add_argument("--tokens", type=int, default=16)
    parser.add_argument("--steps", type=int, nargs="+", default=[4, 8, 16])
    parser.add_argument("--skip-layers", type=int, nargs="+", default=[0])
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--prompts", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--table-only", action="store_true")
    args = parser.parse_args()
    for steps in args.steps:
        transfer_schedule(args.tokens, steps)
    if args.repetitions < 1 or args.warmup < 1 or any(k < 0 or k > 14 for k in args.skip_layers):
        parser.error("Require repetitions/warmup >= 1 and skip counts in 0..14")
    args.output_dir.mkdir(parents=True, exist_ok=False)
    if args.table_only:
        atomic_json(args.output_dir / "candidate-plan.json", {
            "kind": "planning-only-not-measured", "model_id": MODEL_ID,
            "total_nonembedding_params_b": 7.03, "active_params_b_approx": 1.4,
            "vram_fit": "unknown until concurrent CUDA measurement",
            "historical_ar_reference": "docs/results/validation-01.json",
            "historical_reference_is_matched_baseline": False})
        return
    prompts = json.loads(args.prompts.read_text(encoding="utf-8")) if args.prompts else [
        {"id": "fighter-validation", "prompt": VALIDATION_PROMPT}]
    if not isinstance(prompts, list) or not prompts or any(
            not isinstance(p, dict) or not isinstance(p.get("prompt"), str) or not p.get("id") for p in prompts):
        raise ValueError("Prompt file must contain objects with unique id and prompt")
    if len({p["id"] for p in prompts}) != len(prompts):
        raise ValueError("Prompt IDs must be unique")
    status = {"success": None, "phase": "loading", "completed_samples": 0}
    atomic_json(args.output_dir / "status.json", status)
    try:
        runtime = load_runtime(model_id=args.model, revision=args.revision,
                               device=args.device, four_bit=not args.no_4bit,
                               cache_dir=args.cache_dir, local_files_only=args.local_files_only)
        import torch
        configuration = {**vars(args), "output_dir": str(args.output_dir),
                         "prompts": prompts, "kind": "standalone-characterization",
                         "time_utc": datetime.now(timezone.utc).isoformat(),
                         "source": source_manifest(), "model": runtime.metadata}
        atomic_json(args.output_dir / "config.json", configuration)
        conditions = [(s, k) for s in args.steps for k in args.skip_layers]
        for steps, skip in conditions:
            for _ in range(args.warmup):
                runtime.generate(prompts[0]["prompt"], tokens=args.tokens, steps=steps, skip_layers=skip)
        with (args.output_dir / "samples.jsonl").open("x", encoding="utf-8") as samples:
            for repeat in range(args.repetitions):
                order = conditions if repeat % 2 == 0 else list(reversed(conditions))
                for steps, skip in order:
                    for prompt in prompts:
                        if (args.output_dir / "STOP").exists():
                            raise RuntimeError("STOP marker: stopped between complete requests")
                        torch.cuda.reset_peak_memory_stats(runtime.device)
                        start = time.perf_counter_ns()
                        result = runtime.generate(prompt["prompt"], tokens=args.tokens,
                                                  steps=steps, skip_layers=skip)
                        row = {"repetition": repeat + 1, "prompt_id": prompt["id"],
                               "steps": steps, "skip_layers": skip,
                               "request_wall_ms": (time.perf_counter_ns() - start) / 1e6,
                               "peak_allocated_bytes": torch.cuda.max_memory_allocated(runtime.device),
                               "peak_reserved_bytes": torch.cuda.max_memory_reserved(runtime.device),
                               **result.to_dict()}
                        samples.write(json.dumps(row, allow_nan=False) + "\n")
                        samples.flush()
                        status.update(phase="measuring", completed_samples=status["completed_samples"] + 1)
                        atomic_json(args.output_dir / "status.json", status)
                        print(json.dumps({k: row[k] for k in ("repetition", "prompt_id", "steps",
                                                              "skip_layers", "request_wall_ms", "content")}), flush=True)
        status.update(success=True, phase="complete")
    except BaseException as exc:
        status.update(success=False, phase="failed", error=f"{type(exc).__name__}: {exc}")
        raise
    finally:
        atomic_json(args.output_dir / "status.json", status)


if __name__ == "__main__":
    main()
