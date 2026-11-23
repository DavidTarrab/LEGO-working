"""Compare actual output IDs with pinned publisher code and exercise CUDA holds."""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path
import threading
import time

from profile_dllm import atomic_json, source_manifest
from runtime import (GenerationCancelled, MODEL_ID, MODEL_REVISION, MASK_ID,
                     VALIDATION_PROMPT, load_runtime)

REFERENCE_README_SHA256 = "0f5471a047dc45d8783cec77a06b69ae4a5ef951be6d8e832e48a6ae71fec723"


def publisher_reference(readme: Path):
    """Execute only the three reviewed functions, from exactly the pinned README.

    InclusionAI publisher sampler is Apache-2.0. It remains in local model cache,
    and is not replaced with a second copy of our adapter for equivalence tests.
    """
    source = readme.read_bytes()
    if hashlib.sha256(source).hexdigest() != REFERENCE_README_SHA256:
        raise ValueError("Publisher README checksum differs from reviewed revision")
    block = next(b.split("```", 1)[0] for b in source.decode().split("```python\n")[1:]
                 if "def get_num_transfer_tokens" in b and "def generate(" in b)
    parsed = ast.parse(block)
    names = {"add_gumbel_noise", "get_num_transfer_tokens", "generate"}
    functions = [n for n in parsed.body if isinstance(n, ast.FunctionDef) and n.name in names]
    if {n.name for n in functions} != names:
        raise ValueError("Missing reviewed reference functions")
    import torch
    import torch.nn.functional as F
    import numpy as np
    namespace = {"torch": torch, "np": np, "F": F}
    exec(compile(ast.Module(body=functions, type_ignores=[]), "publisher-reference", "exec"), namespace)
    return namespace["generate"]


def run_thread(runtime, **config):
    results, failures = [], []
    def run():
        try:
            results.append(runtime.generate(VALIDATION_PROMPT, **config))
        except BaseException as exc:
            failures.append(exc)
    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    with runtime.gate.condition:
        if not runtime.gate.condition.wait_for(lambda: runtime.gate.running, 120):
            raise RuntimeError(f"Worker did not begin generation: {failures}")
    return worker, results, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", required=True)
    parser.add_argument("--reference-readme", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--tokens", type=int, default=8)
    parser.add_argument("--steps", type=int, default=4)
    parser.add_argument("--skip-pause-tests", action="store_true")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    status = {"success": None, "phase": "load"}
    atomic_json(args.output_dir / "status.json", status)
    runtime = None
    try:
        reference = publisher_reference(args.reference_readme)
        runtime = load_runtime(cache_dir=args.cache_dir, local_files_only=True)
        import torch
        atomic_json(args.output_dir / "config.json", {
            "source": source_manifest(), "model": runtime.metadata,
            "reference_readme_sha256": REFERENCE_README_SHA256,
            "kind": "cuda-correctness-not-performance", "tokens": args.tokens,
            "steps": args.steps})
        prompts = [VALIDATION_PROMPT, "Reply with exactly the word dodge.",
                   "What is two plus two? Answer briefly."]
        checks = []
        for i, prompt in enumerate(prompts):
            status.update(phase="reference-comparison", prompt=i)
            atomic_json(args.output_dir / "status.json", status)
            actual = runtime.generate(prompt, tokens=args.tokens, steps=args.steps)
            formatted = runtime.tokenizer.apply_chat_template(
                [{"role": "system", "content": "You are a helpful AI assistant."},
                 {"role": "user", "content": prompt}], add_generation_prompt=True, tokenize=False)
            ids = runtime.tokenizer(formatted, return_tensors="pt", add_special_tokens=False).input_ids.to(runtime.device)
            expected = reference(runtime.model, ids, steps=args.steps, gen_length=args.tokens,
                                 block_length=args.tokens, temperature=0., cfg_scale=0.,
                                 remasking="low_confidence", mask_id=MASK_ID)
            runtime.synchronize()
            expected_ids = expected[0, ids.shape[1]:].cpu().tolist()
            row = {"prompt": prompt, "match": actual.canvas_ids == expected_ids,
                   "expected_canvas_ids": expected_ids, "actual": actual.to_dict()}
            checks.append(row)
            atomic_json(args.output_dir / "reference-checks.json", checks)
            if not row["match"]:
                raise AssertionError("Publisher output token IDs differ")
            print(json.dumps({"reference_prompt": i, "match": True, "content": actual.content}), flush=True)
        controls = {}
        if not args.skip_pause_tests:
            # Request pause after the first synchronized step. The next gate
            # acknowledges it before any subsequent step is submitted.
            def request_pause(step, _):
                if step == 0:
                    runtime.gate.pause()
            worker, values, failures = run_thread(runtime, tokens=args.tokens,
                                                   steps=args.steps, on_step=request_pause)
            try:
                if not runtime.gate.wait_paused(120):
                    raise AssertionError("Pause was not acknowledged")
                before = runtime.gate.status()["completed_chunks"]
                time.sleep(0.1)
                after = runtime.gate.status()["completed_chunks"]
                if before != after or before != 1:
                    raise AssertionError("Progress changed during acknowledged CUDA hold")
                runtime.gate.resume()
                worker.join(120)
                if worker.is_alive() or failures:
                    raise AssertionError(f"Resume failed: {failures}")
                controls["pause_resume_output_match"] = values[0].canvas_ids == checks[0]["actual"]["canvas_ids"]
                if not controls["pause_resume_output_match"]:
                    raise AssertionError("Paused output differs")
                controls["frozen_completed_chunks"] = before
            finally:
                runtime.gate.stop()
                worker.join(120)
            worker, values, failures = run_thread(runtime, tokens=args.tokens,
                                                   steps=args.steps, on_step=request_pause)
            try:
                if not runtime.gate.wait_paused(120):
                    raise AssertionError("Stop test did not reach paused boundary")
                runtime.gate.stop()
                worker.join(120)
                controls["stop_while_paused"] = (not worker.is_alive() and not values and
                                                  len(failures) == 1 and isinstance(failures[0], GenerationCancelled))
                if not controls["stop_while_paused"]:
                    raise AssertionError("Stop while paused failed")
            finally:
                runtime.gate.stop()
                worker.join(120)
            controls["recovery_after_stop"] = runtime.generate(
                VALIDATION_PROMPT, tokens=args.tokens, steps=args.steps).canvas_ids == checks[0]["actual"]["canvas_ids"]
            if not controls["recovery_after_stop"]:
                raise AssertionError("Generation did not recover after stop")
        atomic_json(args.output_dir / "control-checks.json", controls)
        status.update(success=True, phase="complete", reference_prompts=len(checks),
                      pause_tests_run=not args.skip_pause_tests)
    except BaseException as exc:
        status.update(success=False, phase="failed", error=f"{type(exc).__name__}: {exc}")
        raise
    finally:
        if runtime:
            runtime.gate.stop()
        atomic_json(args.output_dir / "status.json", status)


if __name__ == "__main__":
    main()
