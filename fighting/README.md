# Original gameplay-quality evaluation

This directory preserves the original LLM gameplay competition harness. It is
separate from the supported Windows rendering/inference scheduling experiment.
Its gameplay and model-quality results have not been rerun in that validation.

Follow the vendored `colosseum/README.md` for the game/service environment and
dependencies. Obtain game assets/ROMs separately under their terms. The model
server requires `MODEL_PATH`; no machine-specific fallback is provided.

Run `run_fighting.sh` once per terminal with the desired `MODEL_PATH`,
`CUDA_VISIBLE_DEVICES` and `PORT`. It defaults to loopback binding, GPU 0 and
port 11434. `run_fighting_ace.sh` is a compatibility entrypoint using the same
explicit configuration. Opponent/client settings must agree with the chosen
ports. These shell entrypoints do not configure the Windows scheduling runtime.
