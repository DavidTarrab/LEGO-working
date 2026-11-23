# Checked diffusion inference experiments

The current runtime supports the reviewed LLaDA-MoE-7B-A1B-Instruct revision
`783d3467f108d28ac0a78d3e41af16ab05cabd8d`. It verifies `<|mask|> = 156895`,
formats instruction prompts with the pinned chat template, and uses the
publisher's deterministic low-confidence transfer schedule with one canvas block.
Generation is full-depth unless an explicit experimental skip count is supplied.

The HTTP server provides unrestricted inference and manual cooperative controls.
**Manual pausing and external game contention are not game-controlled scheduling.**
The next integration milestone is admission from actual rendering state, with
bounded compute chunks and matched unrestricted controls.

## Environment and correctness

Use Python 3.12 and the pinned packages. For the tested Windows CUDA environment:

```powershell
python -m pip install torch==2.7.1 --index-url https://download.pytorch.org/whl/cu128
python -m pip install -r dllm/requirements.txt
```

`requirements-windows-cu128.lock.txt` records the complete resolved environment;
use the CUDA index as an extra index when installing that lock. Set `LEGO_PYTHON`
to this interpreter. An isolated local interpreter is supported without changing
the system Python. Dot-source `tools/Enter-LegoDllmEnvironment.ps1` to keep caches
local; `LEGO_STORAGE_ROOT` may point to an existing dependency directory.

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\tools\Enter-LegoDllmEnvironment.ps1
& $env:LEGO_PYTHON -m unittest discover -s dllm -p 'test_*.py'
& $env:LEGO_PYTHON dllm/environment_check.py --output runs/env-01.json
& $env:LEGO_PYTHON dllm/fetch_model.py --cache-dir "$env:HF_HOME\hub" --manifest runs/download-01.json
```

The fetcher downloads immutable files without executing model code and records
SHA256 digests, including weights. On this machine the ordinary HTTP transfer
stalled; an `hf_xet==1.2.0` retry is recorded in the implementation log. Transfer
transport is independent of model correctness. The environment helper currently
selects ordinary HTTP; override `HF_HUB_DISABLE_XET=0` after loading it to use Xet.

`validate_cuda.py` compares full output canvases on three prompts with the actual
publisher sampler extracted from its reviewed, checksum-verified README. Obtain
that README at the pinned model revision, not a changing `main` revision.

```powershell
& $env:LEGO_PYTHON dllm/validate_cuda.py --cache-dir "$env:HF_HOME\hub" `
  --reference-readme path\to\pinned\README.md --output-dir runs/correctness-01
```

This also checks acknowledged mid-generation holds, unchanged output IDs after
resume, stop while paused, and recovery for a subsequent request. It is a
correctness experiment; its timings include instrumentation and first use.

## Repeated characterization

```powershell
& $env:LEGO_PYTHON dllm/profile_dllm.py --cache-dir "$env:HF_HOME\hub" `
  --local-files-only --tokens 16 --steps 4 8 16 --repetitions 3 `
  --output-dir runs/profile-01
```

Every condition is warmed, condition order reverses on alternate repetitions,
and raw JSONL samples retain output IDs, actual counts, transfers, individual
step wall durations, memory peaks, and source hashes. `status.json` updates after
each request; a `STOP` file stops before the next request. Existing directories
are refused. A partial/failed run is retained and marked failed.

Step budgets larger than output length are rejected instead of running forwards
after the canvas has filled. Nonzero `--skip-layers` values are experiments and
do not imply acceptable response quality. Module/config restoration is tested.
`--table-only` emits a clearly labeled planning record, not a measured memory fit.

The old MPS results and figures remain historical artifacts in `docs/results/`.
They are not target-GPU baselines. Dream, generic unknown-model sampling, and the
old clean-prompt heatmap are outside the checked runtime; add them through
separate model-specific validation rather than a logits/mask fallback.

The helper uses `max_split_size_mb:128` unless an allocator setting is already
supplied, and the loader records it. The validated pilot reduced reservation from
about 10.14 to 4.80 GiB without changing tested output IDs. Read the
[implementation results](../docs/DLLM_IMPLEMENTATION_RESULTS_2026-11-23.md) for
the evidence and the separate live-allocation/residency definitions.

## Serving and external contention

```powershell
& $env:LEGO_PYTHON dllm/dllm_server.py --cache-dir "$env:HF_HOME\hub" `
  --local-files-only --steps 4 --port 8080
```

The server binds to loopback. `GET /health` includes actual placement,
quantization, model revisions, and timing semantics. `POST /completion` accepts
the existing load-generator fields. Unsupported options return errors.
Only one thread may own model generation; health and controls stay responsive.

```powershell
Invoke-RestMethod http://127.0.0.1:8080/control -Method Post -ContentType application/json `
  -Body '{"action":"pause","wait_ms":5000}'
# Inspect paused=true for acknowledgement; request acceptance alone is insufficient.
Invoke-RestMethod http://127.0.0.1:8080/control -Method Post -ContentType application/json -Body '{"action":"resume"}'
```

`stop` releases a paused request and stops at a synchronized boundary. Each new
request resets stop state. Acknowledgement covers this worker's submitted CUDA
work, not all work by other GPU processes. No executing kernel is preempted.

`measure_http.py` supports both this server and llama-server. It measures latency
from scheduled arrival, logs all offered/skipped arrivals and full responses,
and preserves diffusion-specific fields. `Run-LegoExternalTrial.ps1` warms the
server, starts the rendering-only packaged game, anchors requests to the game's
QPC start, verifies the game covers request drain, and recrops PresentMon frames
to that exact inference interval. The existing server must already be ready.

```powershell
# Administrator PowerShell is required for Windows ETW frame capture.
.\tools\Run-LegoExternalTrial.ps1 -OutputDirectory runs/external-01 `
  -GameExe path\to\DialogueDemo.exe -Seconds 30 -APM 100 -DiffusionSteps 4
```

Keep the game foreground and unobscured, including two seconds before the window.
`-SkipPresentMon` is an inference/capture-debug option and produces no FPS result.
Each trial reports execution success separately from eligibility for frame
comparison. Focus sampling cannot prove continuous visibility.

`prompt_ms` compatibility fields now mean tokenization time; `predicted_ms` means
whole diffusion generation wall time, including admission holds. The analyzer
labels these explicitly and suppresses autoregressive decode-rate comparisons.
Use end-to-end latency for the common application comparison, and independently
evaluate action quality before claiming useful game-action throughput.

For a preliminary paired AR/diffusion capture batch, use
`tools/Run-LegoDllmCaptureBatch.ps1` from Administrator PowerShell, supplying
`-ValidationDirectory` for a completed clean batch and `-GameExe` for the corrected
package. Its default 20 APM is a feasible smoke demand, not the 200 APM target.
The helper records short fixed-order repeats and does not declare paper readiness.

See [implementation notes](../docs/IMPLEMENTATION_LOG.md) and
[paper experiment protocol](../docs/EXPERIMENT_PROTOCOL.md) for current evidence,
limitations, and the next steps.
