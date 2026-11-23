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
