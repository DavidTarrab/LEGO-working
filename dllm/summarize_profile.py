"""Export selected portable pilot evidence and a standalone latency figure."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def load_batch(batch):
    if read(batch / "status.json").get("success") is not True:
        raise ValueError("Refuse an incomplete or failed validation batch")
    config = read(batch / "profile" / "config.json")
    if config["source"]["dirty"]:
        raise ValueError("Portable clean-batch export requires a clean source revision")
    rows = [json.loads(line) for line in (batch / "profile" / "samples.jsonl").read_text().splitlines()]
    conditions = []
    for steps in config["steps"]:
        selected = [r for r in rows if r["steps"] == steps]
        if len(selected) != 3 or any(r["unresolved_masks"] for r in selected):
            raise ValueError("Pilot export expects three complete samples per step budget")
        if len(config["prompts"]) != 1 or config["skip_layers"] != [0]:
            raise ValueError("This export schema is for the full-depth one-prompt pilot")
        latency = [r["request_wall_ms"] for r in selected]
        conditions.append({"steps": steps, "samples": len(selected),
                           "request_mean_ms": statistics.mean(latency),
                           "request_stdev_ms": statistics.stdev(latency),
                           "request_min_ms": min(latency), "request_max_ms": max(latency),
                           "individual_request_ms": latency,
                           "generated_tokens": [r["generated_tokens"] for r in selected],
                           "peak_allocated_bytes": max(r["peak_allocated_bytes"] for r in selected),
                           "peak_reserved_bytes": max(r["peak_reserved_bytes"] for r in selected),
                           "output_examples": sorted({r["content"] for r in selected}),
                           "canvas_ids": selected[0]["canvas_ids"],
                           "output_ids_identical_across_repeats": len({tuple(r["canvas_ids"]) for r in selected}) == 1})
    checks = []
    for steps in config["steps"]:
        reference = read(batch / f"correctness-{steps}" / "reference-checks.json")
        if len(reference) != 3 or not all(row["match"] for row in reference):
            raise ValueError("Publisher equivalence checks are incomplete")
        checks.append({"steps": steps, "requested_tokens": config["tokens"],
                       "prompts_checked": len(reference), "all_canvas_ids_match_publisher": True})
    controls = read(batch / "correctness-4" / "control-checks.json")
    if not all(controls.get(key) for key in ("pause_resume_output_match", "stop_while_paused", "recovery_after_stop")):
        raise ValueError("Cooperative control checks failed")
    model = dict(config["model"])
    if "load_cache_released" in model:
        # Legacy field described invoking empty_cache, not a measured reduction.
        model["unused_load_cache_release_invoked"] = model.pop("load_cache_released")
        model["load_cache_reduced_reservation"] = (model["loaded_reserved_bytes"] <
                                                   model["reserved_before_load_cache_release_bytes"])
        model["cuda_allocator_config"] = None
    return {"source_revision": config["source"]["git_revision"], "source_clean": True,
            "source_sha256": config["source"]["source_sha256"], "model": model,
            "prompt": config["prompts"][0]["prompt"], "requested_output_tokens": config["tokens"],
            "reference_checks": checks, "controls": controls, "conditions": conditions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch-directory", type=Path, required=True)
    parser.add_argument("--baseline-batch", type=Path)
    parser.add_argument("--upstream-digests", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--date", required=True)
    args = parser.parse_args()
    data = load_batch(args.batch_directory)
    digests = read(args.upstream_digests)
    if digests.get("success") is not True:
        raise ValueError("Upstream weight hashes were not verified")
    data.update(date=args.date, kind="preliminary-standalone-characterization", schema_version=1,
                export_generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                upstream_weight_hashes=digests["checked"], game_controlled_scheduling_implemented=False,
                limitations=["One prompt and three warmed samples per step budget; no confidence interval or tail claims",
                             "Publisher reference and adapter use the same NF4 model instance; this does not establish BF16 quality equivalence",
                             "Fighter output at four steps is malformed; semantic action quality is not established",
                             "No matched game-frame or autoregressive performance result in this standalone record",
                             "Framework allocation/reservation are not measured dedicated GPU residency"])
    if args.baseline_batch:
        data["previous_allocator_batch"] = load_batch(args.baseline_batch)
        data["allocator_comparison_note"] = "Sequential pilot batches; memory accounting and output checks, not a statistical latency comparison"
    args.output_directory.mkdir(parents=True, exist_ok=False)
    (args.output_directory / f"dllm_cuda_validation_{args.date}.json").write_text(
        json.dumps(data, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.size": 10, "svg.fonttype": "none"})
    fig, axis = plt.subplots(figsize=(6.4, 3.8), layout="constrained")
    for item in data["conditions"]:
        axis.scatter([item["steps"]] * 3, [v / 1000 for v in item["individual_request_ms"]], color="#1764a0", s=24, zorder=3)
    axis.plot([s["steps"] for s in data["conditions"]],
              [s["request_mean_ms"] / 1000 for s in data["conditions"]],
              color="#1764a0", linewidth=1, label="Mean of three requests")
    axis.axhline(.3, color="#ad2e24", linestyle="--", linewidth=1, label="300 ms target at 200 APM")
    axis.set(xlabel="Denoising steps", ylabel="End-to-end request time (seconds)",
             xticks=[4, 8, 16], ylim=(0, 9), title="LLaDA-MoE NF4 on RTX 5070 Ti")
    axis.grid(axis="y", alpha=.2)
    axis.legend(frameon=False, loc="upper left")
    fig.text(.5, -.02, "Standalone pilot: one fighter prompt, 16 output slots; individual requests. Quality is not established.",
             ha="center", fontsize=8)
    for suffix in ("svg", "png"):
        fig.savefig(args.output_directory / f"dllm_cuda_latency_{args.date}.{suffix}", dpi=180, bbox_inches="tight")
    print(json.dumps({"source_revision": data["source_revision"], "conditions": data["conditions"]}, indent=2))


if __name__ == "__main__":
    main()
