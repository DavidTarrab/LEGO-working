# Investigating scheduling delays

`Analyze-LegoProfiles.mjs` reads a directory of trial captures, compares host
service/admission times, window focus, calibration and optional admission
diagnostics, and writes Markdown/JSON reports plus SVG duration charts.

```powershell
node .\tools\Analyze-LegoProfiles.mjs .\runs\investigation
```

Subdirectories are discovered by `experiment.json`. Keep complete captures;
individual reports and the raw logs remain useful for checking validity.

## Capturing diagnostics

Build the updated source integration into a separate package to preserve a
historical benchmark binary:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\tools\Enter-LegoEnvironment.ps1
node .\tools\Prepare-LegoIntegration.mjs stage --apply
.\tools\Build-LegoPackage.ps1 -ArchiveDirectory .\local\packaged-lego-profile
$game = Join-Path $PWD 'local\packaged-lego-profile\WindowsNoEditor\DialogueDemo\Binaries\Win64\DialogueDemo.exe'
.\tools\Run-LegoTrial.ps1 -GameExe $game -Mode InterFrame `
  -OutputDirectory .\runs\investigation\inter-profile -Seconds 20 -APM 150 `
  -ExtraGameArguments '-LegoAdmissionProfile'
```

Rendered PresentMon capture requires an Administrator terminal. If investigating
an inference/capture failure independently, `-SkipPresentMon` keeps rendering,
LLM diagnostics and NVML logging but produces **no presentation-FPS measurement**.
Native RHI frame counts are a different measurement. `-Insights` additionally
records an Unreal trace; it is inventoried rather than decoded by these tools.

The runner samples foreground/minimized state at approximately 1 Hz in
`focus.csv`. Keep the scene stationary and window focused/unobscured for
comparisons. Sampling cannot prove continuous focus or visibility between polls.
The NVML logger also records GPU clocks, temperature and performance state;
these are coarse device observations, not per-kernel timings.

## Presentation-only capture and repeated diagnostics

If real-time PresentMon does not produce complete game frames, use a current
PresentMon console executable with the minimal WPR profile shipped here:

```powershell
.\tools\Run-LegoDiagnostics.ps1 -GameExe $game `
  -OutputDirectory .\runs\diagnostics\repeat-01 -Seconds 30 -Repetitions 2 `
  -APM 150 -PresentMon .\local\tools\PresentMon-2.6.0-x64.exe `
  -PresentMonV2 -WprPresent -IncludeChunkComparison
```

Use absolute paths for `-PresentMon`. Download the standalone console from the
[official releases](https://github.com/GameTechDev/PresentMon/releases).
`-PresentMonV2` selects its double-dash arguments and legacy CSV metrics.
GPU/display-completion and input tracking are disabled: these captures measure
application presentation intervals, not successful display or GPU execution.
The selector normalizes the newer lowercase `ms` column prefixes, filters by
the actual game PID and rejects a capture missing either measurement boundary
by more than 250 ms.

`-WprPresent` (alias `-WprGpu`) records DXGI events from `LegoPresent.wprp` and
decodes them after game exit. It requires elevation and refuses to take over
an existing WPR recording. Its automatic idle check currently requires the
English WPR status message; an unrecognized message causes a safe refusal.
All temporary trace files and final ETL files are inside the trial directory.
The optional `-WprProfile GPU` on `Run-LegoTrial.ps1` selects the much heavier
full GPU profile. Measure its overhead before using results as benchmarks.

The diagnostics runner compares polling and notification wakeups for all three
policies. `-IncludeChunkComparison` adds eight-node groups with the original
polling policy. `-IncludeAblations` instead adds those cases, logging removal,
fit-check removal, and an unrestricted control without checkpoints. It reverses
the entire order on alternate repetitions. Create a file named `STOP` in the
batch directory to finish the current trial and refuse the next launch.

No-checkpoint inference still uses this build's disabled CUDA graphs; it is not
a benchmark of a separately tuned upstream server. Eight-node groups change
observation granularity and calibration coverage. Compare them against an
unrestricted control with the same grouping, and validate frame behavior.

## CUDA and graphics timelines

Build the optional NVTX annotations using the NVTX3 headers provided by Nsight:

```powershell
.\tools\Build-LegoInference.ps1 -NvtxIncludeDirectory $nvtxHeaders
# Repackage or restage the matching native DLL before running the game.
$env:LEGO_INFERENCE_NVTX = '1'
.\tools\Run-LegoTrial.ps1 -GameExe $game -Mode InterFrame -Seconds 8 -APM 150 `
  -OutputDirectory .\runs\diagnostics\cuda-inter -SkipPresentMon -Insights `
  -Nsight $nsys -ExtraGameArguments '-LegoAdmissionProfile'
python .\tools\Analyze-LegoGpuTrace.py .\runs\diagnostics\cuda-inter
Remove-Item Env:\LEGO_INFERENCE_NVTX
```

`$nsys` is the Nsight Systems CLI executable; `$nvtxHeaders` is its directory
containing `nvtx3/nvToolsExt.h`. Tracing launches CUDA, NVTX, DX12 and WDDM
collection, exporting both `.nsys-rep` and SQLite. The runner shows the child
game's Unreal window even if the hidden profiler passes a hidden-window startup
flag. Keep it focused and check `focus.csv`; historical hidden-window captures
cannot stand in for focused trials.

The Python tool uses only the standard library and aligns exported timestamps
through Windows QPC and Nsight `systemClockNs`. It checks measured NVTX action
and chunk counts against the native summary, reports CUDA kernel durations,
DX12 workload spans, host observation ranges, API calls and capture warnings,
and exports a 500 ms SVG timeline. Uncovered spans are not proof of idle hardware.
Overlapping GPU spans do not establish simultaneous SM execution. Profiling can
substantially change request latency and frame pacing; use separate captures
without Nsight to assess performance.

## Admission diagnostics

`-LegoAdmissionProfile` enables `admission-waits.csv` and
`admission-summary.csv` in the native run directory. Diagnostics reset after
warmup. Counters are exact; detailed wait rows are bounded to 50,000, with omitted
rows counted. The instrumentation itself adds overhead, so compare an
uninstrumented control using the same executable.

Summary reason masks describe conditions observed **before** each blocked wait:
bit 1 = policy requires outside-frame admission; bit 2 = GPU pending;
bit 4 = RHI pending; bit 8 = estimated chunk does not fit the remaining budget.
Conditions overlap. Mask 8 alone indicates a fit-only rejection. Poll counts are
not duration attribution: rendering state can change while the thread sleeps.
Sleep durations include wakeup/reacquisition delay and are not GPU kernel times.

The analyzer checks that reason-mask and sleep-histogram totals match the poll
counter. Unknown estimates, omitted rows and requested polling interval remain
visible in the raw summary.

Diagnostic ablations (pass as an array through `-ExtraGameArguments`):

| Flag | Change |
| --- | --- |
| `-LegoAdmissionPollUs=50` | Change requested polling wait; Windows may wake much later |
| `-LegoProfileFreezeCalibration` | Preserve warmup calibration during measurement; does not remove warmup outliers |
| `-LegoProfileNoChunkRecords` | Disable detailed chunk record construction, retaining aggregate counters and calibration |
| `-LegoAdmissionEventWake` | Wake on render/RHI/GPU state changes, retaining a timed fallback and the same admission predicate |
| `-LegoProfileNoFit` | Remove the outside-frame budget-fit check; diagnostic only, with frame-overrun protection removed |
| `-LegoCheckpointNodes=8` | Observe every eight graph nodes rather than every node |
| `-LegoCheckpointNodes=0` | Disable checkpoints; allowed only for unrestricted configuration |

These flags are experimental changes, not default performance recommendations.
Record identical focus/workloads and use repeated counterbalanced trials before
attributing small differences. A missed period can also skip the next arrival
under the existing single-request pacing rule, amplifying throughput changes.

Notification wakeups use a generation captured before admission state is read.
The wait checks that generation under the notification mutex, preventing a
render-completion notification between the state check and sleep from being lost.
Notifications can also wake for a change that still does not permit admission.
The profile's historical `polls` fields count wait-loop iterations for both modes;
notification-mode iterations are not independent rendering episodes.

## Completion-fence correctness

The original completion worker copied a `TRefCountPtr` to a UE fence derived from
non-atomic `FRefCountedObject`. Concurrent worker and RHI reference operations
were unsafe. The corrected hook borrows the manager-owned fence without changing
its reference count on worker threads. The manager drains workers before
destroying the fence and counter. Staging upgrades the known old hook with exact
anchor checks and refuses the upgrade if the shutdown drain is absent.

Rebuild editor binaries too before using editor-based tests; updating source
hooks alone does not repair an already built editor or an old package.

Historical results predate this correction. Successful short runs do not prove
absence of a concurrency defect; rerun with the corrected hooks before treating
them as reliable performance baselines.
