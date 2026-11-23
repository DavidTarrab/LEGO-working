# Diffusion implementation record

## 23 November 2026

Work begins on `implementation/dllm-correctness`, based on `f22419b` after the
diffusion merge. The original outer workspace is preserved. The maintained
publication checkout is the source of new implementation changes.

Imported the previously local Windows diagnostics as a coherent source change:
guarded borrowed-fence ownership migration with teardown draining, render-state
generation notifications, admission diagnostics, bounded capture selection,
foreground logging, minimal DXGI capture, and trace/profile analyzers. The
corresponding 20 JavaScript tests and two Python trace tests pass here. These
tests do not rebuild an old editor or prove that any packaged binary matches
the imported source. Reuse of binaries requires hash/source provenance.

The initial Python environment is an isolated workspace Python 3.12.10 runtime.
The system Python alias is unusable. Python/runtime downloads and package caches
remain in ignored local storage. CUDA dependencies and model revisions will be
recorded from actual execution, not inferred from requested CLI settings.

Primary correctness work: a revision-pinned LLaDA-MoE adapter, checked mask ID,
publisher-compatible deterministic transfer schedule, honest output/timing
fields, responsive independent controls, and reproducible individual samples.
Unrestricted HTTP serving does not yet constitute game-controlled scheduling.

### Completed correctness and infrastructure work

- Downloaded the model at `783d3467f108d28ac0a78d3e41af16ab05cabd8d` and
  matched all three weight-shard SHA256 digests to upstream LFS metadata.
- CUDA 12.8 / PyTorch 2.7.1+cu128 / bitsandbytes 0.48.2 NF4 kernel execution
  passed on the RTX 5070 Ti. The resolved Python environment is locked in
  `dllm/requirements-windows-cu128.lock.txt`.
- Built `local/packaged-lego-dllm-20261123` from the staged corrected engine
  modules and synchronized project. No legacy completion-fence hook was pending
  migration. The source engine remains the reviewed UE 4.27.2 revision.
- A new native AR check passed exact output IDs and acknowledged mid-decode
  holds. Raw evidence is `runs/dllm-20261123/ar-native-correctness` in the outer
  workspace. Its timings are correctness instrumentation, not paper baselines.
- Added an explicit pinned diffusion adapter, actual output-count/timing fields,
  fail-fast schedule/options validation, restored layer skipping on exceptions,
  responsive health/manual controls, and an offered-arrival ledger using QPC.
- Added a shared-window external contention runner, with owned-process cleanup,
  full drain coverage checks, frame recropping and separate frame/focus validity.
  Windows ETW capture requires an Administrator terminal and remains to be run.
- Eleven diffusion tests (including CPU numerical and live stub HTTP tests),
  21 JavaScript tests, and two Python trace tests pass. Stub/numerical fixtures
  are test evidence, not target-model performance.

### Issues encountered and corrected

The sandbox Windows profile cannot load Unreal AutomationTool; the package was
built under the normal user profile. This required process-local Git trust
exceptions for existing workspace checkouts, not global Git configuration.

Ordinary HTTP weight transfer stalled. The Xet retry completed; partial files
and failed run directories were retained. `HF_HUB_DISABLE_XET=0` enabled the retry.
The first validation batch also exposed a PowerShell logging hazard: redirecting
native stderr through a pipeline with Stop error preference can terminate on a
traceback/warning before preserving it. The new Python batch captures child logs
directly and refuses later stages after a failed task.

Fresh-process CUDA allocator-stat reset failed before lazy CUDA initialization.
The loader now initializes the context explicitly. An initial assertion also
confused adjacent tokenizer entries: `[gMASK]` is 156894; the publisher's actual
diffusion token is `<|mask|>` at 156895, exposed by `mask_token_id`. The corrected
assertion checks that exact ID/name/property before loading weights. These failures
are in `cuda-correctness`, `cuda-correctness-02`, and `batch-03` under the run root.

`batch-04` then passed publisher-equivalent full canvas IDs on three prompts at
8 output positions / 4 steps, frozen progress at an acknowledged hold, identical
resumed output, stop while paused, and recovery. Its nine-sample, dirty-tree
16-position sweep completed, but it is preliminary characterization, not a final
paper comparison. Per-request times were roughly 2 / 4 / 8 seconds at 4 / 8 / 16
steps; the four-step fighter output contained `Iodge`, so speed cannot be treated
as demonstrated game-action quality. Actual loaded allocation was about 4.72 GiB.

That sweep exposed about 10 GiB of allocator reservation retained from loading,
despite only about 4.72 GiB of live weights. The loader now releases unused load
cache before measurement and records both reservation states and load peak.
Checkpoint completeness and actual quantized-module coverage are also recorded.
The next clean-source batch must revalidate all three step budgets at 16 output
positions and repeat timings after this memory change.

### Next work and interpretation

The Python eager MoE path is currently far above the 300 ms / 200 APM target.
Its roughly 500 ms whole denoising steps cannot fit ordinary 60 FPS rendering
gaps. This is a measured implementation limit, not proof that diffusion as an
architecture is slower. Profile and improve backend/chunk overhead before
promising a useful headroom scheduler. Manual pause tests establish cooperative
correctness only; no renderer-to-diffusion admission bridge exists yet.

Continue with a clean-source CUDA batch, aligned unrestricted game captures at
a feasible demand level, per-model matched AR controls, and quality evaluation.
Do not use the historical MPS figures or original 20-second Windows results as
matched comparison baselines. See `docs/EXPERIMENT_PROTOCOL.md` for paper criteria.
