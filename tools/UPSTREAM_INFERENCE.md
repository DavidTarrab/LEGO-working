# Upstream inference checkpoints

The replacement backend builds `lego-inference.dll`, `lego-infer.exe` and an
NVML sampler against upstream llama.cpp commit
`0a1ef309cdbb9f2024f58a38fb4c42352c5db569`. The source remains unmodified. Build
instructions are in [WINDOWS_SETUP.md](../WINDOWS_SETUP.md).

## Pause/resume semantics

Upstream `llama_context_params.cb_eval` asks which nodes to observe, executes a
graph range, synchronizes the selected backend, then invokes the observation
callback. The worker can wait and resume by returning normally. Returning false
cancels graph evaluation; it is not used as a resumable pause.

The pinned revision executes decode with `llama_process` and
`LLAMA_PROCESS_TYPE_DECODE`. Checkpoints occur inside one such call, including
prefill and token generation. A pause cannot preempt an executing kernel. A graph
operation can launch multiple kernels. An acknowledged pause means the worker
has reached the gate after the observed range, not that the entire GPU is idle.
Input/reset transfers and other processes are outside that guarantee.

`--nodes 1` selects the finest observation granularity; larger values group graph
nodes, not transformer layers. `--nodes 0` uses callback-free graph evaluation
without in-decode pausing. CUDA graph replay is disabled for all these modes.

## Verification

```powershell
.\tools\Build-LegoInference.ps1 -CudaArchitecture 120
.\tools\Test-LegoInference.ps1
.\tools\Test-LegoInference.ps1 -CheckpointNodes 8
.\tools\Test-LegoInferenceControl.ps1
```

The tests use `LEGO_MODEL_PATH` or `local/models/model.gguf`, with optional
`-Model` and `-OutputDirectory` overrides. Native verification holds pauses during
prefill and token decoding, checks that progress is frozen, and compares all
output token IDs with an uninterrupted greedy reference. These checks passed
on the validated model/prompt; they do not prove equivalence for every model,
partitioning choice or prompt. External control tests include stop while paused.

Results are written to new directories under `runs/checkpoint-tests` and
`runs/control-tests`. Host monotonic timing is not CUDA-event kernel timing.

## Manual external controls

From the repository root, terminal A:

```powershell
. .\tools\Enter-LegoEnvironment.ps1
$runner = Join-Path $env:LEGO_UPSTREAM_BACKEND_DIR 'lego-infer.exe'
& $runner --model $env:LEGO_MODEL_PATH `
  --output (Join-Path $PWD 'runs\manual-01') `
  --tokens 256 --nodes 1 --control-name demo
```

After `READY control=demo`, terminal B:

```powershell
.\tools\Set-LegoInferenceControl.ps1 -Name demo -Action Pause -WaitPausedMs 5000
.\tools\Set-LegoInferenceControl.ps1 -Name demo -Action Resume
.\tools\Set-LegoInferenceControl.ps1 -Name demo -Action Stop
```

Stop releases a paused gate, finishes the current decode and stops at a decode
boundary. Pauses do not expire automatically. Controls are scoped to the Windows
logon session; use a unique name per runner. PowerShell controls are experimental
controls, not a mechanism for dispatch into sub-millisecond rendering gaps.

## Unreal bridge

`tools/native/LegoInference.h` exposes a C ABI. One worker owns create/run/destroy;
another thread may request pause/resume/stop. Never destroy concurrently with
run. Event strings must be copied during the callback.

`admission_check` is emitted while graph observation points are selected;
`chunk_complete` follows observed-range completion. Multiple admission checks
can precede one range, so they are not kernel-start timestamps. The Unreal
bridge calls `FTaskScheduler::WaitForAdmission()` directly on its inference
thread and reports observed completion. No external process signalling is
needed for integrated admission. See [STEP6.md](STEP6.md).

The old incomplete custom-backend wrappers are not part of the supported
build. Trained adaptor loading and adaptive layer skipping remain unimplemented.
