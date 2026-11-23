# Working on LEGO experiments

This checkout is the maintained `DavidTarrab/LEGO-working` project. In the original
workspace it lives at `local/github-publication/LEGO`; the outer checkout has a
different Git history and pre-existing work. Do not reset or overwrite that work.

Read `docs/DLLM_PROJECT_STATUS_2026-11-23.md` and `docs/IMPLEMENTATION_LOG.md`
before continuing diffusion work. Keep the end goal a reproducible paper comparing
game rendering and useful on-time inference at a stated response-quality level.

Correctness precedes timing. Never call a stub, synthetic model, MPS measurement,
or historical capture a new CUDA/game result. Separate unrestricted co-location,
manual cooperative pausing, and admission controlled by the game scheduler.
Preserve failed and excluded runs with reasons; do not select only favorable runs.

Keep weights, dependencies, raw captures, and machine paths in ignored `local/`
and `runs/`. Publish only selected portable evidence with revision/configuration
provenance. Do not overwrite experiment directories. Do not change global Python
or Git configuration to make a local experiment work.

Checks: `node --test tools/*.test.mjs`,
`python -m unittest discover -s dllm -p "test_*.py"`,
`python -m unittest discover -s tools -p test_gpu_trace.py`,
`node tools/Check-Publish.mjs`, and `git diff --check`.
GPU tests must record actual model revision, placement, precision, driver, and
output IDs; portable checks cannot establish GPU equivalence or memory fit.

For long batches write durable progress/status files and logs, wait for completion
at useful boundaries, and avoid rapid polling. Keep the user informed when a
material result or blocker changes the next action.
