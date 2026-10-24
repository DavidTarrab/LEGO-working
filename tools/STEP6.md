# Integrated runtime and experiment details

The supported build/run commands are in [README.md](../README.md) and
[WINDOWS_SETUP.md](../WINDOWS_SETUP.md). This document describes what they do.

## Runtime sequence

1. Open the UE Third Person example map at its initial camera, disable VSync
   and apply the requested FPS cap.
2. Load the external GGUF through `LegoUpstreamModel` and `lego-inference.dll`.
3. Run three unrestricted warmup actions through the same checkpoint path.
   Discard the first for calibration; use the remaining actions to initialize
   per-phase/node high-water host duration observations.
4. Offer actions at the configured APM on one inference worker. Latency starts
   at the scheduled arrival. Stale arrivals are skipped rather than queued.
5. Gate observed graph work through the selected policy and record completions.
6. Record the measurement window in raw QPC ticks, flush results and exit.

Policy 2 is unrestricted. Policy 1 waits outside the frame for no observed
GPU/RHI work and enough estimated remaining headroom. Policy 0 additionally
permits observed inside-frame gaps. The default admission timeout is two seconds;
it cancels rather than waiting forever. Scheduled modes reject absent hooks.

## Rendering observations

The staging tool adds frame boundaries, RHI activity scopes, and D3D12 command
submission/completion tracking to a separately obtained licensed engine.
Submission stays GPU-pending until its completion fence is observed. Manager
teardown drains completion workers before destroying fence resources.

These aggregate signals can hide smaller rendering gaps. They do not measure
individual shader durations or guarantee when the next rendering work arrives.
A clear snapshot is not proof that all GPU engines are globally idle.

## Calibration and granularity

All comparison modes use one-node graph observation with CUDA graphs disabled.
The scheduler updates a per-phase/node high-water host duration estimate and uses
an inter-frame fit margin of 0.05 ms. Unknown nodes use a reference estimate.
Blocked admission checks run at approximately 200 microsecond intervals, subject
to Windows wakeup latency. These are engineering heuristics, not formal bounds.

The history predictor exists in the source but does not drive current admission.
The runtime does not reproduce the paper's next-window headroom planning,
minimum-gap duration constraint, or adaptive layer/sublayer execution groups.
No trained FFN adaptors or adaptive layer skipping are active.

## Output

```text
trial/
  experiment.json                 # configuration, hardware and launch metadata
  game.log, stdout.log, stderr.log
  frames-raw.csv                  # rendered modes only
  frames.csv, frames.window.json  # selected measurement window + provenance
  gpu.csv
  llm/<timestamp-guid>/           # inference modes only
    config.json, summary.json, measurement.json
    llm.csv, gpu.csv, chunks.csv
    calibration-initial.csv, calibration-final.csv
  render/measurement.json         # rendering-only mode
  analysis/report.html           # inference modes
  analysis/metrics.json, summary.md, charts/*.svg
```

`Run-LegoMatrix.ps1` runs five modes per repetition, rotates concurrent policy
order, and regenerates concurrent reports with that repetition's LLM-only and
rendering-only baselines. It writes `status.json`, `comparison.csv`,
`comparison.json` and `summary.md` at the matrix root. Trial directories must be
new; existing captures are never overwritten. Render-only data is summarized
by the matrix without requiring an inference CSV.

PresentMon 1.5.2 captures raw QPC values. `Select-LegoMeasurement.mjs` selects the
game's measurement interval and removes the first boundary interval per
swapchain. `frames.window.json` records which measurement file was used.
The NVML sampler flushes each 200 ms sample and stops through a named event,
avoiding buffered-tail loss. GPU samples are cropped for analysis.

## Configuration

Generated `local/DialogueUpstream/Config/DefaultGame.ini` contains `[LEGO]`:
`ModelPath`, `PromptFile`, `OutputDirectory`, `Seconds`, `APM`, `FPS`, `Tokens`,
`Context`, `OperationPolicy`, `WarmupActions`, `AdmissionTimeoutSeconds`.
Repackage after changing packaged defaults. The game accepts corresponding
`-LegoModel`, `-LegoPrompt`, `-LegoOutput`, `-LegoSeconds`, `-LegoAPM`, `-LegoFPS`,
`-LegoTokens`, `-LegoPolicy` and `-LegoWarmup` overrides. `Context` and admission
timeout currently come from the configuration file.

The trial runner controls standard duration/APM/FPS/output arguments. Configure
other workload changes consistently across baselines and all policies. The
default prompt is short, not the paper's 512-token workload. Verify actual prompt
and output counts in `llm.csv`; generation can terminate before the token limit.

## Interpretation and limits

Prompt/decode durations include admission waits and are host wall time, not pure
GPU durations. The first output token comes from prefill; integrated decode
rates count subsequent tokens. The chunk timeline is a deterministic reservoir
of up to 100,000 observations across the run; aggregate counters remain exact.
Its gaps do not imply GPU idleness. Admission snapshots can race with new
rendering work and cannot establish kernel overlap or a scheduling violation.

The preliminary measurements in the README establish a functioning pipeline
and a rendering/LLM tradeoff. They do not show simultaneous attainment of both
targets. Longer repeated trials, representative scenes, finer GPU timing,
normal-upstream overhead comparisons and scheduling refinements remain necessary
before making paper-equivalent claims. Commercial-game trace replay is separate
future work; original collector CSVs do not supply that implementation.
