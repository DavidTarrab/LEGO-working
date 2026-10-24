# Concurrent-run analysis

`Analyze-ConcurrentRun.mjs` produces offline HTML reports, SVG charts, JSON
metrics and Markdown summaries. It requires Node.js 18+ and no installed packages.
It reads both integrated runtime output and the optional external-server
`Measure-LlmLoad.ps1` format. It never contacts an inference server.

## Analyze a trial

```powershell
node .\tools\Analyze-ConcurrentRun.mjs .\runs\intra-01 `
  --llm-baseline .\runs\llm-only --game-baseline .\runs\game-only `
  --gpu-utc-offset -04:00 --fps 60
```

Use the offset recorded at capture time, not a guessed current offset.
`Run-LegoTrial.ps1` runs analysis automatically; `Run-LegoMatrix.ps1` also adds
matching baselines and a cross-mode summary. Open `analysis/report.html`.
Re-running analysis replaces generated reports, not raw measurements.

```text
trial/
  experiment.json                 # recommended provenance
  frames.csv                      # optional PresentMon 1.x capture
  frames.window.json              # optional QPC-selection provenance
  game.utrace                     # optional, inventoried but not decoded
  llm/<timestamp>/
    llm.csv
    config.json, summary.json     # strongly recommended
    gpu.csv                       # optional NVML/NVIDIA CSV
    measurement.json              # integrated QPC interval
    chunks.csv                    # optional sampled host observations
```

A trial/baseline must identify one LLM capture. If multiple timestamps exist,
select one using `--llm-dir` or pass an exact baseline timestamp directory.
The game baseline requires `frames.csv`, not just `.utrace`.
`node tools/Analyze-ConcurrentRun.mjs --help` lists filters/output options.

## Report contents

- Frame timeline/CDF, p50/p95/p99, worst intervals, dropped presents and fractions
  exceeding the exact, 1.5× and 2× budgets. These are application presentation
  intervals, not GPU rendering durations. Exact-budget crossings include jitter.
- LLM latency/deadlines, achieved APM, skipped arrivals, prompt/output counts,
  prompt/decode durations and completion bins. Skips are separate from deadline
  misses among completed actions; inspect both.
- Integrated service, admission wait and arrival lateness, plus a sampled chunk
  timeline. Host prompt/decode times include admission waits. The first generated
  token comes from prefill; integrated decode throughput counts subsequent tokens.
  External server timing/accounting has different semantics.
- Device-wide utilization, allocation, power and sampled VRAM headroom. Missing
  counters are excluded rather than treated as zero. Coverage is reported.
- Matching-baseline percentile/rate changes and recorded configuration warnings.
  Descriptive changes do not establish statistical significance or GPU causality.

Percentiles use nearest rank. Chart reduction preserves bucket first/min/max/last
points, while statistics use every valid selected row. Multiple applications,
PIDs or swapchains are never pooled; the largest matching stream is selected
and disclosed unless filters override it. Malformed optional frame/GPU records
are skipped with warnings; malformed LLM records fail analysis.

## Clocks and coverage

Integrated capture uses PresentMon 1.5.2 `-qpc_time`. `Select-LegoMeasurement.mjs`
selects the completed game QPC window, excludes startup/warmup and drops the
first boundary interval per swapchain. `frames.window.json` records provenance.
Shared frame/inference timing is claimed only when its measurement file matches
the analyzed run. Without that provenance, relative frame and inference clocks
are separate and exact cross-stream alignment is not inferred.

LLM timestamps are UTC; GPU timestamps are local without an offset. Supply
`--gpu-utc-offset` for portable interpretation. A capture should not span a
wall-clock/DST discontinuity. GPU analysis selects the LLM measurement interval
and warns about incomplete coverage. NVML logging flushes each row and stops
through a named event. The optional older external-server sampler can lose its
buffered tail at shutdown; prefer integrated capture for GPU coverage.

Chunk durations are host submission-to-observed-completion measurements. A
bounded deterministic reservoir retains at most 100,000 observations across the
run, with exact aggregate counts recorded separately. Its gaps do not prove
idle time. Admission-state snapshots can race with new rendering submissions;
they do not establish kernel overlap or scheduling violations.

Unreal Insights must decode `game.utrace` separately. Coarse GPU counters and
these CSVs cannot establish CUDA/graphics kernel overlap or per-layer GPU timing.

## Verification

```powershell
node --test .\tools\Analyze-ConcurrentRun.test.mjs .\tools\Select-LegoMeasurement.test.mjs
```

Tests cover parsing, percentile/accounting rules, interrupted records, clock
provenance, integer-precision QPC cropping, stream selection, baseline handling,
report generation, integrated waits and sampled chunk warnings.
