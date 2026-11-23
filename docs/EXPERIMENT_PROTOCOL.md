# Protocol for the autoregressive and diffusion comparison

The paper should test whether cooperative scheduling improves the joint tradeoff
between rendering and useful on-time inference, and whether diffusion offers a
better measured tradeoff than the chosen AR system. A negative or neutral result
is valid. Meeting a frame cap in an already uncongested scene is not a scheduling
speedup. MPS results cannot establish Windows CUDA performance.

## Evidence stages

1. Environment smoke: actual CUDA and NF4 execution, versions, device capability.
2. Model correctness: publisher-equivalent output IDs, valid mask completion,
   actual quantization/placement, pause/resume and stop/recovery checks.
3. Standalone characterization: repeated warmed samples with task-quality data.
4. Unrestricted contention: game only, model only, and both, on common QPC windows.
5. Game-controlled scheduling: measured render admission and bounded compute,
   with output equivalence, shutdown safety, and checkpoint overhead controls.
6. Final paired matrix: repeated workload/demand sweeps with quality and frame
   constraints fixed before choosing a preferred configuration.

Do not relabel data from an earlier stage as a later-stage result. In particular,
manual HTTP controls and separate-process contention do not establish stage 5.

## Workload and metric definitions

Record prompt text and token counts for each tokenizer. Use the existing fighter
prompt as a compatibility workload, then a frozen multi-state task set and a
separate dialogue workload. Chat templates and precision/quantization are part
of the model configuration. LLaDA-MoE NF4 versus Llama GGUF is a comparison of
complete systems; it does not isolate the inference architecture alone.

Latency begins at scheduled arrival and ends at final usable response. Record
dispatch lateness and service time separately. There is one outstanding request;
arrivals missed while busy are skipped, never silently queued. Retain an offered
arrival ledger and finish an already-running request after the arrival window.
The game must remain active during that drain; include drain in aligned frame
and elapsed-time measurements. Reject incomplete coverage.

Report completed APM, completed-action deadline misses, skipped arrivals, errors,
and on-time completions divided by all offered arrivals. HTTP success and an
action parser are not a semantic quality score. Quality-qualified APM additionally
requires a frozen acceptance rubric applied to every candidate completion.

Diffusion full-canvas generation time is distinct from AR prefill/decode. Actual
visible output counts exclude EOS/special tokens; requested canvas positions and
all raw IDs remain available. Per-step p95 within one request is not request p95.
Reciprocal service time is not an achieved offered-load rate.

Presentation intervals are not pure GPU render times or proof of display. Report
FPS, p50/p95/p99 intervals, long-frame fractions against declared thresholds,
capture coverage, and render-only deltas. A capped average of 60 FPS does not
prove every interval is below 16.67 ms. Coarse NVML samples cannot prove kernel
causality or overlap. Keep Nsight/full GPU tracing separate from benchmark runs.

## Controls and provenance

Freeze source/model/tokenizer/remote-code revisions, weight hashes, executable
and DLL hashes, driver, environment lock, device placement, attention/cache
settings, quantization, scene/camera, resolution, frame cap, and power/thermal
observations. No shader/build work may overlap timed runs. Store source hashes
even when a smoke run uses a dirty tree, and mark such runs explicitly.

Compare normal model-only execution, checkpointed unrestricted execution, and
each scheduled policy with identical backend settings. Measure checkpoint/IPC
overhead independently. Use a deterministic heavier scene as well as the light
smoke scene; sweep offered demand through saturation. Use matching per-model
standalone controls. Run AR and diffusion sequentially, each sharing the GPU
with the game, unless simultaneous model residency is the question being tested.

For paper measurements, begin with at least three counterbalanced 120-second
repetitions, repeated baselines, stable foreground before warmup, and sufficient
sample counts for the requested tail. Increase repetitions when variability or
rare events demand it. Publish individual trial results and run-level variation;
do not average per-run percentiles and call them a pooled tail.

## Failures and eligibility

Preserve failed and excluded directories with their status and reason. Require
complete PID-filtered frame coverage and foreground/non-minimized samples during
the measured interval and two-second lead-in. Sampling is not proof of continuous
visibility. Window switches, missing GPU/frame tails, unexplained errors, model
offload, and mixed timing semantics require investigation before inclusion.

Public summaries must contain only selected portable fields. Raw traces, model
weights, licensed engine content, process-local paths, and caches stay ignored.
The implementation log should connect each claimed result to its raw run and
state exactly which stage it supports.
