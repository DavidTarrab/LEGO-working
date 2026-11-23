# LEGO diffusion LLM project status and implementation plan

Date: 23 November 2026. Target: run a diffusion language model alongside a real game on one gaming GPU, schedule its compute around rendering, and compare its latency, useful throughput, rendering impact, memory use, and response quality with an autoregressive model.

The project has a working Windows autoregressive scheduling prototype and a newly merged standalone diffusion profiling and HTTP serving prototype. **It does not yet have a diffusion model connected to the rendering scheduler, or a controlled diffusion versus autoregressive co-scheduling result.** The next milestone is a correct, reproducible CUDA diffusion baseline and synchronized game contention experiment; cooperative diffusion scheduling follows that baseline. Existing MPS diffusion measurements and historical Windows autoregressive numbers cannot establish the final comparison.

## 1 Repository identity and merge scope

The active project on GitHub is [DavidTarrab/LEGO-working](https://github.com/DavidTarrab/LEGO-working). The workspace's outer Git repository still points to [sjtu-epcc/LEGO](https://github.com/sjtu-epcc/LEGO), the original research artifact. They are different histories and different states of the implementation.

The requested diffusion branch is named `yuan-dllm-eval`. Before this merge, GitHub `main` was `887e14f9a0d13f123e157500be4c7eac2a95b335`; the diffusion tip was `83dfa7f4960f2ba5c02faf56e45a1e961572c388`. The branch was three commits ahead with no commits behind and added seven files without changing the existing runtime:

| Commit | Contribution |
| --- | --- |
| `903f660` | Standalone diffusion profiler, layer-skipping evaluator, HTTP server |
| `8b7cf8a` | Correction for tuple-returning MoE layers in the skipping experiment |
| `83dfa7f` | MPS characterization and filtered ModuleList implementation |

The branch was merged into `main` in the clean publication checkout at `local/github-publication/LEGO`, using merge commit `db283e9d21e866a838ed80aa24144232d4e8222d`. This report is a subsequent documentation change. The branch's code and recorded results were preserved; the issues identified below were not silently rewritten during the merge.

The outer workspace had 288 modified, deleted, or untracked status entries before this review. It contains Windows diagnostics and correctness changes newer than published `main`. Those pre-existing changes were preserved and were not included wholesale in the branch merge. Consequently, **merging the diffusion branch does not publish the newer Windows fixes**. Section 5 identifies the relevant changes that should be reviewed and ported deliberately.

## 2 What currently works

### 2.1 Autoregressive execution and scheduling

The supported Windows path uses UE 4.27.2, a separately obtained licensed source engine, a packaged Third Person scene, and upstream llama.cpp pinned to `0a1ef309cdbb9f2024f58a38fb4c42352c5db569`. `tools/native/LegoInference.cpp` exposes the C ABI in `LegoInference.h`; the Unreal bridge in `LegoUpstreamModel` invokes the scheduler on its inference worker.

The backend supports cooperative pause and resume at observed graph ranges, including inside prompt processing and token decoding. It cannot interrupt an already executing CUDA kernel. The implementation disables CUDA graph replay so replay does not bypass observation points. All three integrated scheduling policies share this backend:

| Policy | Current behavior |
| --- | --- |
| Unrestricted, 2 | Permit checkpointed inference without rendering restrictions |
| Inter-frame, 1 | Wait outside the frame for clear tracked GPU/RHI state and enough estimated remaining frame budget |
| Intra-frame, 0 | Also permit admission inside a frame when tracked GPU/RHI work is clear |

`tools/Run-LegoTrial.ps1`, `Run-LegoMatrix.ps1`, `Select-LegoMeasurement.mjs`, and `Analyze-ConcurrentRun.mjs` provide launch, measurement selection, baseline comparison, and offline reports. Integrated runs record a shared QPC measurement window, action CSVs, summaries, calibration, and sampled chunk records. These are substantial building blocks for the diffusion experiment.

This remains a scheduling prototype. The active runtime does not implement the paper's complete headroom prediction, adaptive layer/sublayer partitioning, resource-oriented trained adaptors, or commercial-game trace replay. `FTaskScheduler::SetSkipDepth` rejects nonzero skipping; the adaptive skip update methods keep depth zero. Existing adaptor and fighting code is separate research infrastructure, not an already connected solution.

The current widget supplies a fixed fighter-state prompt and displays returned text. It does not establish a live feedback loop in which actual scene state drives model decisions and those decisions control gameplay. That loop is not required for the immediate scheduling comparison, but an eventual NPC/gameplay claim needs state extraction, a validated action parser, game-thread dispatch, stale-action handling, and task-level evaluation.

### 2.2 Diffusion additions

[`dllm/profile_dllm.py`](../dllm/profile_dllm.py) supplies device selection, model loading, an estimated candidate-memory table, step-budget profiling, filtered transformer-layer experiments, and a representation-similarity heatmap. [`dllm/dllm_server.py`](../dllm/dllm_server.py) supplies `/health` and `/completion` endpoints compatible at the field-name level with `tools/Measure-LlmLoad.ps1`.

There are two generation paths. A model exposing `diffusion_generate` uses its native generator. Other models use the repository's explicit masked-token loop. Both currently execute an entire request without receiving game admission decisions. There is no shared-memory control, native scheduler call, rendering-state subscription, or pause acknowledgement in the diffusion path.

The HTTP path is therefore a useful starting point for **unrestricted separate-process contention**, not an implementation of LEGO diffusion scheduling. The integrated matrix still runs the native autoregressive backend; pointing a diffusion server at port 8080 does not change that matrix.

## 3 What the measurements establish

### 3.1 Published autoregressive validation

[`docs/results/validation-01.json`](results/validation-01.json) records one 20-second trial per mode at 200 APM, 60 FPS, 1920 by 1080, a stationary template camera, three warmup actions, a short prompt, and at most 16 output tokens, on an RTX 5070 Ti. Its approximate results are:

| Mode | Achieved APM | Completed-action deadline misses | Skipped arrivals | Presentation FPS | Frame p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Render only | — | — | — | 59.98 | 17.44 |
| LLM only | 200.57 | 0% | 0 | — | — |
| Unrestricted | 137.39 | 45.65% | 21 | 48.46 | 30.65 |
| Inter-frame | 101.19 | 100% | 33 | 60.00 | 17.23 |
| Intra-frame | 101.20 | 100% | 33 | 60.00 | 17.35 |

These establish pipeline execution and a rendering/inference tradeoff under that capture. They do not establish simultaneous 60 FPS and 200 on-time actions per minute. Focus was not monitored, the weight checksum is absent, and the runs predate the completion-fence ownership correction present in the outer workspace. Rebuild and rerun before treating these numbers as a dependable comparison baseline.

### 3.2 Newer local Windows diagnosis

The outer workspace contains `runs/diagnosis-20261028/FINDINGS.md`, its individual captures, and `local/diagnostic-summary.json`. These are local engineering evidence, not artifacts added by the diffusion merge. The retained comparison used the corrected fence hook, two 20-second trials per condition, 150 APM with 400 ms deadlines, a 36-token prompt, 16 generated tokens, and the same stationary scene. Foreground samples were checked during measurement and the preceding two seconds.

| Observation and wakeup | Unrestricted mean latency ms | Inter-frame mean latency ms | Intra-frame mean latency ms |
| --- | ---: | ---: | ---: |
| One node, polling | 303.76 | 354.53 | 349.51 |
| One node, notifications | 300.84 | 336.74 | 332.13 |
| Eight nodes, polling | 209.50 | 243.86 | 244.29 |

Every retained condition averaged approximately 60 presentation FPS. One-node polling inter/intra each had one miss and one skipped arrival across its two trials; the other retained conditions had none. Frame p99 values were approximately 17.23 to 17.70 ms. These runs differ in load, focus controls, code, and capture procedure from the published validation; they are not a clean numerical before/after comparison with it.

The diagnosis reports 16,016 observed ranges per default action, reduced to 2,002 by eight-node grouping. The approximately 30% latency improvement occurs in unrestricted execution too: it principally demonstrates observation/synchronization overhead, not an advantage attributable only to scheduling. Notification wakeups reduce admission wait, while aggregate rendering counters hide most internal command-list gaps. Local traces also show substantial profiling overhead and background-window distortions.

The practical implication is that the benchmark needs a workload where there is measured contention. At a fixed 150 APM arrival rate and 60 FPS cap, an unrestricted control already completing all requests at about 60 FPS leaves little improvement available on those capped metrics. Scheduling should be judged by its joint quality and latency tradeoff, especially near the capacity limit.

### 3.3 Committed diffusion characterization

[`dllm_week1_4_characterization.json`](results/dllm_week1_4_characterization.json) identifies LLaDA-MoE on `mps`, without four-bit quantization, with 16 layers and a reported 13.7 GiB allocation value:

| Requested steps | Recorded total latency ms | Reported standalone capacity APM | Sample output |
| --- | ---: | ---: | --- |
| 4 | 10,912.81 | 5.50 | Dodge |
| 8 | 21,964.72 | 2.73 | dodge |
| 16 | 44,406.02 | 1.35 | Dodge followed by an explanation |
| 32 | 90,659.20 | 0.66 | Same displayed explanation |
| 64 | 177,449.43 | 0.34 | Same displayed explanation |

These are MPS measurements, not Windows CUDA or game-concurrent results. The file contains only the zero-skipped-layer row, despite the documented 0/2/4/6/8 sweep. It therefore supplies no measured nonzero skipping speed/quality curve. The similarity matrix is 17 by 17 for a model reporting 16 transformer blocks; its hidden-state indices need explicit labels, including embeddings/final normalization as applicable.

The candidate-memory table and autoregressive reference values are literal constants in the profiler, not live measurements or values loaded from the referenced validation JSON. A table-only run emits those constants. No committed diffusion capture establishes a 16 GB CUDA co-location fit or a scheduled-game result.

## 4 Issues that block a valid diffusion comparison

### 4.1 Diffusion sampling correctness must be established first

`run_diffusion_steps` selects the tokenizer's mask ID, then falls back to a model-config value or an EOS/default integer. That fallback is unsafe across model families: a valid token ID is not necessarily the learned diffusion mask token. The publisher's LLaDA-MoE reference uses mask ID `156895` and a model-specific transfer schedule. Add an explicit model adapter, assert the mask ID against the selected tokenizer/revision, and fail if it cannot be resolved. Verify rather than assume that the current tokenizer exposes the right mask property. [Publisher reference sampler](https://huggingface.co/inclusionAI/LLaDA-MoE-7B-A1B-Instruct).

The generic path does not apply the instruction model's chat template. It also assumes any loaded `AutoModel` output is suitable vocabulary logits. If a different model returns hidden features or uses a shifted prediction convention, treating `outputs[0]` as token logits can produce invalid generation. Give each supported model a checked loader, prompt formatter, logits interpretation, mask policy, and stopping policy. Test deterministic output IDs and intermediate canvas states against the publisher's generator on multiple prompts. A raw-prompt compatibility experiment may be retained, but label it separately from properly formatted instruction evaluation.

The transfer rule uses `max(1, max_new_tokens // steps)`. With 16 output positions and ordinary non-mask predictions, the 32-step experiment commits all positions after 16 iterations and then runs 16 forwards with nothing left to unmask; the 64-step experiment runs 48 such forwards. Already committed tokens are never revisited. Thus the current increasing-latency curve partly measures redundant full passes, not progressively more useful refinement. When the lengths are not divisible, the last step can instead receive a disproportionately large remainder.

Replace that rule with a validated per-model schedule. If reproducing a schedule that deliberately includes zero-transfer iterations, preserve and describe it; separately count masked positions, transfer counts, model forwards, and any work after the canvas is filled. Do not call redundant forwards additional quality-improving refinement. Test step budgets both below and above output length, and reject unresolved masks in the returned completion.

The Dream branch hardcodes `temperature=0.2` and does not seed the generator. The load generator sends temperature zero and seed one, which this server ignores. The autoregressive native path uses greedy sampling. Record and honor model-specific generation settings, or explicitly define a repeated stochastic experiment. Randomness must not be an unrecorded difference between full and skipped models.

### 4.2 Current per-step and HTTP metrics are misleading

For Dream, the profiler assigns `[total_ms / steps] * steps` after one opaque generation call. These are invented equal-duration entries; their p95 is not a measured step-tail statistic. Instrument real boundaries, or report only end-to-end generation time until that exists. The publisher exposes generation hooks that can help locate boundaries, but their placement must be checked before using them for admission. [Dream generation implementation](https://huggingface.co/Dream-org/Dream-v0-Instruct-7B/raw/main/generation_utils.py).

The server labels the first denoising pass `prompt_ms` and the rest `predicted_ms`. Diffusion passes repeatedly process the canvas, so these names do not represent autoregressive prefill and decode. `predicted_per_second` then divides the entire requested output budget by time excluding the first pass. That number is not total useful output throughput.

Two accounting defects were confirmed through a local HTTP probe with generation stubbed, without loading weights:

1. A returned one-word completion with a requested 16-token budget still reports `predicted_n=16`. The handler never counts actual visible output tokens, EOS truncation, or unresolved mask/special-token positions. Requested canvas length and usable completion length need distinct fields.
2. With one 100 ms diffusion step, it reports `prompt_ms=100` and `predicted_ms=100`, attributing the same interval twice. The analyzer can consequently produce negative apparent overhead.

Use a common end-to-end request latency for the main AR/diffusion comparison. Add diffusion-specific fields such as generation wall time, real per-step times, executed forwards, committed/remaining positions, tokenization time, admission wait, requested slots, actual output IDs, and stop reason. Retain legacy fields only with explicit semantics. Update `Measure-LlmLoad.ps1` and `Analyze-ConcurrentRun.mjs` so those semantics and model metadata survive into reports; currently the client discards diffusion-only response fields and the analyzer defaults to server-style prompt/decode interpretation.

Define time to first *usable* action separately. A diffusion canvas with tentative tokens does not automatically provide an action the game may safely consume, and the current server returns only a final completion.

### 4.3 The profiler is not yet a benchmark suite

Each step budget currently produces one measured request after one two-step warmup. The reported per-step p95 is a percentile across steps within that request, not a p95 of request latency across trials. `60000 / latency_ms` is a reciprocal service-time capacity estimate, not achieved APM under paced arrivals with rendering.

Add repeated requests across a frozen prompt set, warm each configuration, record seeds and actual token IDs, counterbalance configuration order, and export individual samples. Measure end-to-end host time separately from CUDA-event compute intervals. The generic path's reported total sums selected step intervals and excludes tokenization, initial transfers, canvas setup, text decoding, and some between-step host overhead. HTTP latency includes more of that work. Both are useful if named accurately.

The heatmap currently processes four clean prompts, not intermediate masked canvases across diffusion timesteps. Similarity on those representations does not validate skipping during denoising. Add timestep/mask-density coverage and evaluate actual quality after skipping; representation similarity alone is not an acceptance criterion.

### 4.4 Memory fit and the execution environment remain unvalidated

The table's approximately 5.1 GiB LLaDA-MoE runtime estimate is a planning assumption. MPS current allocation, CUDA maximum allocated memory, reserved allocator memory, total device usage, and a game's dedicated/shared residency are different measurements. The function named `weights_vram_gb` uses framework allocation counters, and the JSON's `GB` fields are computed using powers of 1024. Fix units and distinguish measured quantities from estimates.

MoE active parameters reduce compute, not the need to store inactive experts unless an explicit offload scheme is used. `device_map='auto'` may choose placement incompatible with the intended one-GPU experiment. Inspect and persist actual placement, quantized module coverage, device names, allocated/reserved peaks, and total-device memory. Fail a one-GPU benchmark if it silently uses CPU offload or another GPU, unless that is a separately declared configuration.

The reported `use_4bit` value is derived from the CLI request, although loading only enables NF4 on CUDA. It must record the actual loaded state. The broad `AutoModel`/`AutoModelForCausalLM` exception fallback can also obscure the real load failure. Preserve error causes and distinguish unsupported architecture, dependency incompatibility, memory exhaustion, and quantization failure.

`requirements.txt` contains only lower bounds. Pin a supported Python version and tested PyTorch/CUDA/Transformers/Accelerate/bitsandbytes combination, plus model, tokenizer, and remote-code revisions. Verify four-bit kernels on the RTX 5070 Ti; the native CUDA 12.8 build working does not establish that a Python wheel supports the same hardware. The README's `.venv-dllm` environment name is not covered by the current `.venv`/`venv` ignore entries; fix that before creating an environment in the public checkout.

Measure game-only, model-only, and concurrent peak residency under the intended scene, resolution, context, output length, step count, and instrumentation. Reserve margin for loading, shader work, logits/attention temporaries, allocator peaks, and graphics allocations. The table mentions DLSS, but these committed numbers do not establish a DLSS-enabled workload or its memory cost.

### 4.5 The server needs explicit experimental controls

`HTTPServer` is single threaded. Serial generation is appropriate for the existing one-outstanding-request workload, but a long completion also blocks health/control requests on that server. There is no pause/resume/stop endpoint or independent control thread. Pausing an HTTP client would not pause already queued CUDA work.

Add bounded input validation for steps, tokens, skip count, prompt/context length, and body size. Return structured errors for malformed JSON and inference failure. Reject unsupported request options rather than silently ignoring experimental settings. Keep one owner of mutable model state; if serving becomes concurrent, the temporary ModuleList replacement cannot be modified by competing requests. Use a separate control channel for cancellation/admission so controls remain responsive while inference runs or waits.

## 5 Windows issues that must be reconciled with published main

### 5.1 Completion-fence ownership is a correctness prerequisite

Published `tools/Prepare-LegoIntegration.mjs` generates a worker that copies `TRefCountPtr<FD3D12CommandListFence>`. In the available UE source, the fence derives from `FRefCountedObject`, whose reference count is non-atomic. Concurrent worker/RHI reference operations can race. A shutdown drain alone does not make those reference operations safe.

The outer workspace has a targeted `upgradeFenceOwnership` correction: borrow the manager-owned fence on workers, avoid cross-thread reference-count changes, and require the manager to drain workers before destroying the fence and completion counter. It includes a guarded migration of the known old hook. **Port that correction and its regression test before rebuilding a published scheduled-runtime baseline.** Rebuild native/engine/project/package components as applicable; changing staging source does not repair already built editors or packages. Stress repeated launch/exit and cancellation under load.

### 5.2 Capture and window state need the newer validation

The local diagnosis documents partial PresentMon captures, hidden or background game windows, and very large full-GPU tracing overhead. The newer local runner adds PID filtering, coverage checks at both QPC boundaries, newer legacy CSV normalization, focus logging, and optional minimal DXGI capture/offline decoding. Published main lacks those later additions.

Port a coherent, tested subset of `Run-LegoTrial.ps1`, `Select-LegoMeasurement.mjs`, their tests, and the associated capture support. Use focused and unobscured windows with a stable lead-in before warmup. Separate application presentation rate from displayed frame rate and GPU execution time. Do not accept a successful report build as proof of capture coverage. The diffusion README's hardcoded UTC offset must also be replaced with the offset recorded at capture time; New York is UTC minus five on this report's date.

### 5.3 Scheduler overhead and coarse signals remain material

The default 200 microsecond wait request can wake much later on Windows. The local notification implementation includes a generation counter to avoid losing a state change between admission testing and waiting. Port that mechanism and its concurrency checks before using it as a diffusion control model. Notification loops still need cancellation, timeout, and spurious-wakeup handling.

Per-node synchronization and hot-path CSV string construction are expensive. Explore meaningful bounded chunks and deferred formatting, but compare every change with an unrestricted control using the same observation granularity. The current fit predictor uses high-water host completion times, which can contain scheduling noise. Kernel-only estimates would omit real launch/completion costs; neither should be treated as a hard bound without validation.

The intra-frame predicate permits work when tracked activity is clear; it does not predict how long the next internal gap will last. Aggregate pending-work counters cannot reveal idle gaps within a still-pending rendering batch. A more precise implementation needs finer rendering submission/completion observations and calibrated chunk durations. No policy can promise both targets when demand exceeds available capacity.

## 6 Step by step implementation plan

### Step 1 Establish a reproducible source and binary baseline

Keep `LEGO-working/main` as the experimental source of truth. Review and port the targeted local correctness/capture changes from Section 5 in explicit commits, preserving unrelated research and local work. Record source revision, engine revision, toolchain, driver, model hash/revision, quantization, build flags, executable/DLL hashes, and the capture-tool version per trial.

Add the diffusion directory to CI with a supported Python setup, syntax/import checks, table-only smoke, and focused tests for sampler schedules, output counts, one-step timing, request validation, and skip restoration. The existing published CI only exercises JavaScript analysis/staging and the publication audit; it does not validate a diffusion GPU load or generation. Keep optional GPU validation separate from portable checks.

**Completion condition:** a clean checkout reproduces portable checks; matching Windows artifacts run and exit cleanly with corrected hooks; each result identifies the actual binary and model used.

### Step 2 Make one diffusion model correct on the target CUDA GPU

Start with the branch's LLaDA-MoE model as a provisional candidate, subject to measured fit and quality. Lock its source/tokenizer revisions and implement the validated generation adapter described in Section 4. Compare against its publisher reference on several deterministic prompts, including the existing fighter prompt. Log all output IDs, mask counts, executed forwards, and stopping decisions. Add separate Dream support only after its own conventions are validated.

Run full-depth model-only tests before skipping layers. Measure CUDA generation latency and device residency. Check that no hidden CPU placement or non-target GPU participates. A model that loads but produces invalid actions has not passed this step.

**Completion condition:** correct complete outputs, repeatable generation settings, known quantization and placement, and measured memory fit with margin. Preserve reference-versus-adapter evidence.

### Step 3 Repair metrics and profile feasible configurations

Implement honest diffusion timings and counts, then preserve those fields through the load generator and analyzer. Profile a fixed prompt suite across useful output lengths and step budgets, collecting repeated end-to-end samples and actual step/chunk measurements. Use the same action-quality rubric throughout.

Measure at least request wall time, p50/p95/p99 across requests, peak allocated/reserved/total memory, actual useful output length, unresolved-mask rate, parser validity, and task success. Plot latency versus quality. Treat 16-token outputs as a short-action workload; add a longer NPC-dialogue workload separately rather than assuming its result generalizes.

At 200 APM the nominal interarrival period is 300 ms; at 150 it is 400 ms, and at 100 it is 600 ms. If acceptable-quality standalone inference cannot fit a proposed deadline, scheduling cannot remove its compute requirement. Reduce demand, improve the backend, or choose another quality/step/model setting before expecting that target to succeed alongside rendering.

**Completion condition:** at least one measured configuration suitable for an initial demand level, plus a quality/latency curve showing the tradeoffs. The initial 200 APM goal may legitimately be infeasible and should remain a measured outcome.

### Step 4 Produce aligned unrestricted game contention baselines

Build an orchestrated external-process runner. It must start the server, verify readiness and model metadata, warm the model and scene outside measurement, launch the rendering-only game mode, record process IDs, create a common measurement window, collect frame/GPU/request samples, and clean up both processes. Make the game path explicit: use the separate rendering-only baseline or the packaged `-LegoRenderOnly` mode, with matching scene/settings. Do not depend on implicit package defaults.

Capture game only, diffusion only, AR only, game plus unrestricted diffusion, and game plus unrestricted AR on the same machine. For the external comparison, use a recorded llama-server build with the same one-request, cache-disabled workload conventions. Compare each co-located model with its own standalone run. Keep HTTP transport on both sides for this experiment; do not ascribe HTTP/native differences to architecture or scheduling.

The current manual three-terminal instructions do not align startup, warmup, frame capture, and request measurement. They are useful for smoke testing, but not sufficient for the final result. Add QPC start/end and request events to the external client, propagate measurement boundaries to the server, and crop all streams to the same interval. Count arrivals near the end using a defined completion/drain rule.

**Completion condition:** repeatable, aligned unrestricted contention reports with complete coverage and no accidental second inference workload. This achieves co-location, not yet co-scheduling.

### Step 5 Connect diffusion execution to rendering admission

A practical first design is a Python CUDA worker that owns the diffusion model, controlled by a small native bridge to the existing UE scheduler. Keep model weights resident. The UE side publishes render state or bounded grants; the worker acknowledges a completed safe boundary, waits for admission, executes one chunk, synchronizes the relevant work, and reports completion and duration. Separate control traffic from HTTP completion handling.

Use Windows shared memory/named events or another measured local control mechanism; do not poll HTTP for microsecond-scale gaps. Define request IDs, sequence/generation numbers, monotonic timestamps, cancel/stop states, pause acknowledgements, timeout behavior, and process-death handling. Only one worker owns the mutable canvas/model state. Acknowledged pause must mean no additional inference chunk can be submitted until resumed, after accounting for previously queued work.

Begin with a complete denoising step as the scheduling unit to prove the protocol. Validate paused-versus-unpaused output IDs, frozen progress during holds, resume, stop while paused, engine exit, worker failure, and repeated requests. Synchronization must cover all streams actually used by the selected backend; a host callback before queued CUDA finishes is not a completion acknowledgement.

**Completion condition:** an instrumented game run proves that the diffusion worker obeys real render admission, preserves generation, and shuts down cleanly. Changing only request arrival times or pausing the client does not satisfy this step.

### Step 6 Make diffusion chunks small enough for the game workload

Measure step and kernel tails on CUDA. The MPS step durations cannot determine suitable Windows chunk size. If a full denoising pass exceeds available gaps, step-only admission will either overrun rendering or starve inference. At 60 FPS the nominal frame period is about 16.67 ms, and the usable headroom is only part of that period.

Implement resumable boundaries at transformer blocks or supported attention/MLP suboperations as required. Preserve canvas, timestep, hidden state, attention semantics, MoE routing behavior, RNG state, and original layer identity. Avoid enqueuing the rest of a pass ahead of its admission grants. Forward hooks alone need review: a hook may block the CPU while earlier CUDA work remains in flight. A single oversized kernel cannot be fixed by adding a host gate after it.

Benchmark IPC cost, synchronization cost, host launch overhead, and CUDA compute separately. Calibrate by model, shape, timestep/mask density, layer/chunk, quantization, and skip setting. Compare checkpoint-free and checkpointed unrestricted controls as well as scheduled controls. Do not build thousands of synchronous metadata-only checkpoints by copying the AR path literally.

**Completion condition:** chunk-tail duration plus dispatch/completion overhead fits empirically observed useful gaps, and matched controls quantify the cost of making inference schedulable. If no practical granularity works, document that limit and evaluate a different backend/model rather than reporting a scheduling success.

### Step 7 Add skipping or adaptors only when quality and capacity justify them

First rerun the missing 0/2/4/6/8 skipping sweep. Ensure the requested layers are actually bypassed, original modules/config are restored after success and exception, unsupported layouts fail explicitly, and per-layer cache/index assumptions remain valid. Recalibrate durations for every retained configuration. The current helper silently does nothing if it cannot find a recognized ModuleList.

Replace token-set overlap with task metrics: valid allowed action, action suitability for a labeled game state, explanation consistency, and separately NPC-dialogue quality. Token overlap ignores order and can reward an incorrect answer sharing familiar words. Evaluate varied prompts/seeds and early/middle/late denoising states; do not infer preservation from a clean-prompt heatmap.

If unadapted skipping loses unacceptable quality, train architecture-specific adaptors or investigate a separately evaluated distilled model/step schedule. Existing AR FFN adaptors cannot be assumed compatible with MoE diffusion or its intermediate states. This training is not a prerequisite for the first full-depth co-scheduling result; it becomes necessary if the desired capacity/quality point requires it.

**Completion condition:** a measured quality-preserving configuration, or an explicit documented quality loss. Only then connect adaptive step/skip selection to SLO demand, with static-budget controls to separate scheduling gains from reduced model work.

### Step 8 Run the final controlled AR versus diffusion matrix

Use an explicit manifest per experiment and freeze prompts, game scene/camera path, resolution, quality settings, frame cap, focus procedure, context/output settings, seed policy, caching, admission policy, and measurement duration. Run AR and diffusion sequentially, each with the game on the same GPU; simultaneous residency of both models would be a different experiment.

| Condition | AR | Diffusion | Purpose |
| --- | --- | --- | --- |
| Game only | Shared matched control | Shared matched control | Rendering floor and memory budget |
| Model only, normal backend | Yes | Yes | Practical standalone capability |
| Model only, scheduling checkpoints enabled | Yes | Yes | Instrumentation/checkpoint cost |
| Game plus unrestricted, matched checkpoints | Yes | Yes | Contention control for each scheduler |
| Game plus inter-frame admission | Yes | Yes | Scheduling effect |
| Game plus intra-frame admission | Yes | Yes | Additional internal-gap benefit |
| Step/skip/adaptor ablations | Full-depth and supported variants | Full-depth and supported variants | Quality and reduced-compute effects |

Do not force both architectures to use the same primitive unit: one AR token, one diffusion step, and one graph node have different costs. Match the application task and quality requirement. Report model family/size, total versus active MoE parameters, NF4 versus GGUF quantization, native versus Python backend, and precision as separate factors. A LLaDA-MoE versus Llama result is a system comparison; it cannot isolate architecture alone.

Sweep offered APM through the capacity boundary and use both the stationary smoke scene and a deterministic heavier scene with actual contention. Add a scripted camera/gameplay path, not uncontrolled human input, for repeatability. Start with 120-second measurements and at least three counterbalanced repeats; choose more samples/repeats when estimating rare misses or p99 tails. Rotate model/policy order, repeat baselines, and record temperature/clocks and capture overhead. Use separate profiling runs for mechanism analysis.

The central result should be **maximum sustainable on-time useful APM under a stated rendering and quality constraint**, alongside the full tradeoff curves. Useful APM counts only completed, valid, quality-qualified actions within deadline. Also publish ordinary completed APM, skipped arrivals, timeouts, errors, and quality failures so exclusion does not conceal demand.

**Completion condition:** a complete paired matrix with individual captures, configuration/binary provenance, uncertainty across repetitions, and an interpretation that separates backend/checkpoint overhead, scheduling, reduced diffusion work, and quality changes. A neutral or unfavorable diffusion result is a valid outcome if measured correctly.

## 7 Measurement rules for the final report

For each offered action, preserve scheduled arrival, dispatch, start, completion, deadline, stop/failure reason, and whether it was skipped. Use the same arrival-to-completion latency definition on AR and diffusion. External request-start latency currently omits arrival lateness that the integrated AR path includes; standardize it before comparing tails. Define offered-arrival counting and end-of-window draining so slow configurations cannot improve their rate by omitting unfinished work.

Report both completed-action miss fraction and an offered-work success fraction. At 200 APM, completing about 100 actions on time while skipping the rest is not success at the offered load. A reasonable primary fraction is on-time quality-qualified completions divided by all offered arrivals; report every failure category separately.

For rendering, report presentation FPS, frame-time p50/p95/p99, long-frame fractions against declared thresholds, capture coverage, and render-only deltas. A capped average of 60 FPS does not imply every frame is within 16.67 ms. GPU counters are coarse, often device-wide observations, not proof of simultaneous execution, free GPU capacity, or kernel causality.

For inference, use true end-to-end latency as the comparable application metric. Report AR prefill/decode and diffusion step/chunk details as architecture-specific diagnostics. Preserve raw request samples rather than averaging per-run percentiles into a purported pooled tail. Report run-level variation and paired differences; use enough independent repetitions before making statistical claims.

For quality, freeze a game-state test set and acceptance rubric before selecting the fastest configuration. State whether the result is raw latency, valid-action throughput, or quality-qualified throughput. A one-word correct action can be appropriate for one task while failing a dialogue task, so do not use a universal token count as a quality proxy.

## 8 Priority order and completion checklist

| Priority | Work | Why it comes first |
| --- | --- | --- |
| P0 | Port fence ownership correction and rebuild matching artifacts | Published scheduler correctness prerequisite |
| P0 | Validate mask ID, model generation conventions, transfer schedule, and output completeness | Invalid generation invalidates performance conclusions |
| P0 | Repair counts, one-step accounting, synthetic step timings, and metadata propagation | Current reports can compare unlike quantities |
| P0 | Verify pinned CUDA environment and real concurrent memory fit | MPS results and hardcoded estimates do not establish target feasibility |
| P1 | Port capture/focus validation and automate aligned external controls | Needed for trustworthy contention baselines |
| P1 | Implement and verify cooperative diffusion admission with independent controls | Required to claim co-scheduling |
| P1 | Bound chunk tails and measure checkpoint/IPC overhead | Required for useful scheduling in rendering gaps |
| P1 | Run repeated paired AR/diffusion matrix with task-quality checks | Required for the final performance conclusion |
| P2 | Complete skipped-layer study, then adaptors/adaptive budgets if justified | Capacity improvement with explicit quality cost |
| P2 | Add representative heavier scripted game workloads | Establishes whether findings extend beyond the smoke scene |
| P3 | Commercial-game collection/replay and paper-equivalent reproduction | Separate expansion beyond the immediate AR/diffusion goal |

The final goal is reached when a clean source revision reproduces a target-GPU diffusion run that obeys game rendering admission at verified compute boundaries, and the matched AR/diffusion matrix quantifies rendering impact, useful on-time throughput, memory, and quality. Merely serving diffusion over the compatible HTTP schema or launching it beside a game reaches an earlier milestone.

## 9 Verification performed for this merge and review

The clean publication checkout passed all 15 existing JavaScript tests covering CSV/reporting, measurement selection, and staging/project behavior. `node tools/Check-Publish.mjs` checked 15,322 existing Git-visible files at the merged-code stage with zero findings. `git diff --cached --check` passed before the merge commit. Both diffusion Python sources parsed, and the table-only command completed without downloading weights.

A temporary standard-library HTTP probe used the actual request handler with model generation stubbed. It confirmed the requested-versus-generated count problem and one-step timing duplication described in Section 4. It verifies handler behavior, not model correctness or GPU execution. The newer outer workspace's 20 JavaScript tests also passed separately, including its guarded fence migration and capture-selection cases.

The system `python` command resolved to an unusable Windows application alias; the available embedded UE Python 3.7.7 was sufficient for parsing, table-only generation, and the stub probe, but is not the supported environment for the declared modern ML dependencies. No new model-weight download, four-bit CUDA generation, Unreal rebuild, live game capture, or diffusion pause/resume experiment was performed in this review. Historical native verification and local diagnostics are identified separately above; they are not new execution evidence for the merged diffusion path.

## 10 Source map for implementation

| Concern | Primary files |
| --- | --- |
| Diffusion model, schedule, skipping, profiling | [`profile_dllm.py`](../dllm/profile_dllm.py), [`requirements.txt`](../dllm/requirements.txt) |
| Diffusion HTTP contract | [`dllm_server.py`](../dllm/dllm_server.py), [`dllm README`](../dllm/README.md) |
| External pacing and result fields | [`Measure-LlmLoad.ps1`](../tools/Measure-LlmLoad.ps1), [`EXTERNAL_BASELINE.md`](EXTERNAL_BASELINE.md) |
| Model timing and comparison | [`Analyze-ConcurrentRun.mjs`](../tools/Analyze-ConcurrentRun.mjs) |
| Integrated launch and matrix | [`Run-LegoTrial.ps1`](../tools/Run-LegoTrial.ps1), [`Run-LegoMatrix.ps1`](../tools/Run-LegoMatrix.ps1) |
| Engine staging and fence lifetime | [`Prepare-LegoIntegration.mjs`](../tools/Prepare-LegoIntegration.mjs), its tests, and the later outer-workspace correction |
| Native AR controls and callbacks | [`LegoInference.h`](../tools/native/LegoInference.h), [`LegoInference.cpp`](../tools/native/LegoInference.cpp) |
| Unreal action timing and logging | [`LegoExperiment.cpp`](../scheduler/UnrealEngine/Engine/Source/Runtime/LegoUpstreamModel/Private/LegoExperiment.cpp) |
| Admission, rendering state, and estimates | [`TaskScheduler.cpp`](../scheduler/UnrealEngine/Engine/Source/Runtime/SchedulingSystem/TaskScheduler/Private/TaskScheduler.cpp), [`LegoAdmission.h`](../scheduler/UnrealEngine/Engine/Source/Runtime/SchedulingSystem/TaskScheduler/Public/LegoAdmission.h), `TaskDetector`, `PerformanceDetector` |
| Measured results versus planning constants | [`validation-01.json`](results/validation-01.json), [`diffusion characterization`](results/dllm_week1_4_characterization.json) |

The original paper supplies the broader algorithm/system target: [LEGO, HPCA 2026](https://doi.org/10.1109/HPCA68181.2026.11408477). Its trained adaptor and full headroom scheduler should be treated as additional implementation work, not capabilities inherited merely by using this repository.
