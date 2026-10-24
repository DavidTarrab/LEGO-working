# LEGO: Windows scheduling experiments

This repository contains the research artifact for **LEGO: Supporting
LLM-Enhanced Games with One Gaming GPU** (HPCA 2026), plus a working Windows
experiment harness using Unreal Engine 4.27.2 and upstream llama.cpp.
[Paper](https://doi.org/10.1109/HPCA68181.2026.11408477) ·
[Windows setup](WINDOWS_SETUP.md) · [Analysis guide](tools/ANALYSIS.md)

The supported experiment runs a real Unreal scene and a full LLM on one GPU,
comparing unrestricted inference with inter-frame and intra-frame admission.
It produces frame-time, latency, deadline, GPU-utilization and chunk reports.
The missing original custom inference DLL is replaced by a pinned upstream
backend with verified cooperative checkpoints inside inference.

**This is a scheduling prototype, not a complete reproduction of the paper.**
Adaptive layer skipping and trained FFN adaptors are not connected to this
runtime. Checkpoints have fixed graph-node granularity rather than the paper's
adaptive layer/sublayer grouping. Commercial-game trace replay is not implemented.

The earlier separate-process llama-server contention experiment remains available
as an [optional external baseline](docs/EXTERNAL_BASELINE.md).

## Validated experiments

These experiments completed on Windows with an RTX 5070 Ti (16 GB), driver 610.88,
CUDA 12.8, VS2019/v142, UE 4.27.2 and Llama 3.1 8B Instruct Q4_K_M:

- Mid-decode pause/resume during prefill and token generation: progress remained
  frozen during acknowledged holds and all 16 token IDs matched the uninterrupted
  reference for the tested prompt.
- External pause, resume and stop while paused, with clean shutdown.
- Source-engine and standalone packaged game runs for all three policies.
- PresentMon capture aligned to the game measurement window, complete NVML GPU
  logging, baseline comparisons and offline HTML reports.

A preliminary five-mode run used **20 seconds per mode, 200 APM, 60 FPS,
1920×1080**, a fixed template camera, three warmup actions, a short prompt and
up to 16 output tokens:

| Mode | Achieved APM | Completed-action deadline misses | Skipped arrivals | Average present rate | Frame p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Rendering only | — | — | — | 59.98 FPS | 17.44 ms |
| LLM only | 200.57 | 0% | 0 | — | — |
| Unrestricted | 137.39 | 45.65% | 21 | 48.46 FPS | 30.65 ms |
| Inter-frame | 101.19 | 100% | 33 | 60.00 FPS | 17.23 ms |
| Intra-frame | 101.20 | 100% | 33 | 60.00 FPS | 17.35 ms |

Successful execution does not mean both targets were met. These short runs show
a rendering/inference tradeoff, not statistical significance or reproduction of
the paper's results. A portable numeric record is in
[docs/results/validation-01.json](docs/results/validation-01.json).
Raw runs, licensed engine files, models and machine-specific logs are excluded.

## Requirements

- Windows, a CUDA-capable NVIDIA GPU and compatible driver. The validated GPU
  has 16 GB VRAM; other hardware needs its own validation.
- Git, Node.js 18+, Windows PowerShell 5.1+, Visual Studio 2019 C++ build tools
  with CMake/Ninja, Windows SDK 10.0.19041.0 and .NET Framework 4.6.2 Developer Pack.
- CUDA Toolkit 12.8 and an authorized Unreal Engine 4.27.2 **source** checkout.
  Epic account/GitHub access is required. Engine source and template content are
  not distributed here.
- A GGUF obtained under the model's license. The tested model was Llama 3.1 8B
  Instruct Q4_K_M. Put it at `local/models/model.gguf` or set `LEGO_MODEL_PATH`.
- PresentMon **1.5.2** for the existing CSV schema. Put its executable at
  `local/tools/PresentMon64-1.5.2.exe` or set `LEGO_PRESENTMON`.
  Rendered captures require an elevated PowerShell terminal.

Follow [WINDOWS_SETUP.md](WINDOWS_SETUP.md) to obtain dependencies, build the
source editor, and create Third Person template content before the commands below.

## Build

Run from the repository root; no particular drive or checkout name is required.
Dependencies, caches and build outputs default to ignored `local/`.
[Environment overrides](tools/STORAGE.md) support other installation locations.

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\tools\Enter-LegoEnvironment.ps1

# Fetches the pinned upstream source on first build.
# Architecture 120 is the validated Blackwell setting; select yours as needed.
.\tools\Build-LegoInference.ps1 -CudaArchitecture 120
.\tools\Test-LegoInference.ps1
.\tools\Test-LegoInferenceControl.ps1

node .\tools\Prepare-LegoIntegration.mjs stage --apply
node .\tools\Prepare-LegoIntegration.mjs project
.\tools\Build-LegoEngine.ps1 -Target DialogueDemoEditor `
  -Project (Join-Path $PWD 'local\DialogueUpstream\DialogueDemo.uproject')
.\tools\Build-LegoEngine.ps1 -Target ShaderCompileWorker
.\tools\Test-LegoGame.ps1 -Policy 2
.\tools\Test-LegoGame.ps1 -Policy 1
.\tools\Test-LegoGame.ps1 -Policy 0
.\tools\Build-LegoPackage.ps1
```

`project` requires `local/LegoBaseline/Content` and refuses an existing destination.
On later updates use `node tools/Prepare-LegoIntegration.mjs sync-project
local/DialogueUpstream` instead. Staging applies guarded, idempotent edits to the
licensed engine checkout; it does not overwrite it with a whole engine overlay.

## Re-run the five-mode experiment

Open **PowerShell as Administrator**, change to the repository root, and run:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\tools\Run-LegoMatrix.ps1 `
  -OutputDirectory .\runs\validation-01 `
  -Seconds 20 -APM 200 -FPS 60 -Repetitions 1 `
  -Purpose preliminary-validation
```

This runs rendering only, LLM only, unrestricted, inter-frame and intra-frame
sequentially. The packaged game opens and closes automatically. No separate
llama-server is needed. Choose a new output directory each time. Allow startup
and model loading in addition to the 100 seconds of measurement.

Read `runs/validation-01/summary.md`; open a mode's
`repeat-01/<Mode>/analysis/report.html` for frame/latency distributions and point
tooltips. Reports work offline and include separate SVG charts, JSON metrics and
Markdown summaries. Raw measurements are retained locally.

For longer repeated measurements, keep prompt, model, camera, graphics settings,
window focus and background GPU workloads consistent:

```powershell
.\tools\Run-LegoMatrix.ps1 `
  -OutputDirectory .\runs\matrix-01 `
  -Seconds 120 -APM 200 -FPS 60 -Repetitions 3
```

This is approximately 30 minutes of measurement plus startup and warmup. Policy
order rotates across repetitions and baselines are repeated. Finish builds and
shader compilation before measuring.

To run one mode, change resolution or capture Unreal Insights events:

```powershell
$game = Join-Path $PWD 'local\packaged-lego\WindowsNoEditor\DialogueDemo\Binaries\Win64\DialogueDemo.exe'
.\tools\Run-LegoTrial.ps1 -GameExe $game -Mode IntraFrame `
  -OutputDirectory .\runs\intra-01 -Seconds 120 -APM 200 -FPS 60 `
  -ResX 1920 -ResY 1080 -Insights
```

## Scheduling behavior

| Mode | Policy | Admission |
| --- | ---: | --- |
| Unrestricted | 2 | Same checkpointed backend without rendering restrictions |
| InterFrame | 1 | Outside the frame, no observed GPU/RHI work pending, sufficient estimated remaining time |
| IntraFrame | 0 | Also use observed inside-frame gaps when GPU/RHI work is clear |

The engine tracks frame boundaries, RHI activity and D3D12 completion fences.
Inference can wait before subsequent observed graph work; it cannot preempt an
executing kernel. Some input/reset work happens outside that gate. All modes
use one-node observation with CUDA graph replay disabled, controlling for that
overhead within the policy comparison. Normal upstream inference is a separate
baseline for measuring the checkpoint mechanism's cost.

Warmup calibrates per-phase/node host durations. The current scheduler does not
use the paper's next-window headroom prediction or its explicit minimum-gap
constraint. Observations cannot guarantee when new rendering work will arrive.
See [runtime details](tools/STEP6.md) and [backend semantics](tools/UPSTREAM_INFERENCE.md).

## Reading measurements

- Assess achieved APM, skipped arrivals and completed-action deadline misses
  together. A skipped arrival is not counted as a completed request.
- PresentMon measures presentation intervals, not pure GPU rendering time.
  Small pacing jitter at a capped rate can exceed the exact frame budget.
- Integrated prompt/decode times include host admission waits; they are not
  interchangeable with external llama-server timing fields.
- Frames are cropped using shared QPC ticks. Coarse device-wide GPU counters
  and chunk snapshots cannot establish kernel overlap or GPU causality.
- The template workload is not the paper's 4K commercial-game evaluation or its
  token-count-verified 512-input-token workload.

## Layout

| Path | Contents |
| --- | --- |
| `tools/` | Supported build, verification, capture and analysis commands |
| `tools/native/` | Inference C ABI, standalone runner and NVML sampler |
| `scheduler/DialogueDemo/Source/` | Native Unreal game and UI |
| `scheduler/UnrealEngine/.../Runtime/` | Project-owned inference/scheduling modules; engine edits are generated by staging |
| `docs/` | Portable validation results and publication notes |
| `adaptor/` | Original training/accuracy research code and vendored dependencies; separate from the validated runtime |
| `fighting/` | Original gameplay-quality harness; not validated by these scheduling experiments |
| `games/` | Original collector source and rendering data; not a working commercial-game replay system |
| `local/`, `runs/` | Ignored dependencies, builds, models and raw experiments |

## Checks and publication

```powershell
node --test .\tools\Analyze-ConcurrentRun.test.mjs `
  .\tools\Select-LegoMeasurement.test.mjs .\tools\Prepare-LegoIntegration.test.mjs
node .\tools\Check-Publish.mjs
```

Analysis/staging tests require no GPU or Unreal installation. Native and live
checks above require their respective dependencies. The publish check examines
the current Git-visible tree, not Git history. Read
[docs/PUBLISHING.md](docs/PUBLISHING.md) before distributing a snapshot.

Project code retains the [Apache 2.0 license](LICENSE). Vendored components keep
their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Obtain
Unreal, CUDA, model weights and the paper under their respective terms.
