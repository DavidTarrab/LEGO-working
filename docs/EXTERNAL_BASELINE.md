# Optional external-server baseline

This is the earlier separate-process experiment, before the integrated
checkpoint scheduler. It profiles contention but does not schedule inference
around rendering and has different timing semantics.

Obtain a compatible CUDA build of `llama-server` and matching DLLs from the
official llama.cpp project. Record its release/revision and launch settings.
The bridge build in this repository does not build the server.

Terminal A, from the repository root:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\tools\Enter-LegoEnvironment.ps1
$server = Join-Path $PWD 'local\llama-bin\llama-server.exe'
& $server -m $env:LEGO_MODEL_PATH -ngl 99 -c 2048 -np 1 `
  --host 127.0.0.1 --port 8080
```

Check the log for actual GPU offload. Terminal B:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Invoke-RestMethod http://127.0.0.1:8080/health
.\tools\Measure-LlmLoad.ps1 -Seconds 120 -APM 200 -OutputDir .\runs\external-llm-only
```

For concurrent contention, launch a separately packaged rendering-only
`LegoBaseline` game (created and packaged through the editor). Use its actual
binary path and process name. From an elevated terminal:

```powershell
$game = Join-Path $PWD 'local\packaged\WindowsNoEditor\LegoBaseline\Binaries\Win64\LegoBaseline.exe'
Start-Process -FilePath $game -WindowStyle Normal -ArgumentList @(
  '-dx12','-windowed','-ResX=1920','-ResY=1080','-ExecCmds=t.MaxFPS 60,r.VSync 0')
$trial = Join-Path $PWD 'runs\external-concurrent-01'
New-Item -ItemType Directory -Path $trial
. .\tools\Enter-LegoEnvironment.ps1
& $env:LEGO_PRESENTMON -process_name LegoBaseline.exe `
  -output_file "$trial\frames.csv" -timed 120 -terminate_after_timed -dont_restart_as_admin
```

While capture runs, use another terminal at the repository root:

```powershell
.\tools\Measure-LlmLoad.ps1 -Seconds 120 -APM 200 `
  -OutputDir .\runs\external-concurrent-01\llm
```

The earlier workload also exercised 450 APM. Use fresh directories and matching
baselines for other rates. Manual launch does not align the streams or exclude
startup automatically. A game-only baseline captures the same game without LLM
requests. Analyze using the actual GPU offset at capture time:

```powershell
node .\tools\Analyze-ConcurrentRun.mjs .\runs\external-concurrent-01 `
  --llm-baseline .\runs\external-llm-only --gpu-utc-offset -04:00 --fps 60
```

The older load generator has one outstanding request, skips stale arrivals,
disables prompt caching, and samples with `nvidia-smi`. That sampler can lose its
buffered tail. Inspect coverage, achieved APM and skips. Do not infer causality
from unaligned clocks or attribute integrated/external differences entirely to
scheduling. Prefer the integrated matrix for aligned capture and NVML sampling.
