# Original adaptor research code

This directory preserves the original heatmap, model modification, adaptor
training and accuracy evaluation sources. It is separate from the validated
Windows full-model scheduling runtime; no adaptor weights are provided or loaded
by that runtime.

- `modeling/`: custom model definitions.
- `struct_heatmap.py`, `draw_heatmap.sh`: representation heatmaps. Set `MODEL_PATH`
  and `DATASET_PATH` explicitly before running the shell entrypoint.
- `LLaMA-Factory/`: vendored training framework with research modifications.
  Install its documented dependencies in an isolated Python environment, then
  configure model/dataset locations and training settings in the selected YAML.
- `lm-evaluation-harness/`, `run_test.sh`: separate accuracy evaluation. Set
  `MODEL_PATH`; optionally set `TASKS`, `DEVICE`, `BATCH_SIZE` and
  `TRUST_REMOTE_CODE`. Inspect custom modeling code before enabling remote code.

`train_adaptor.sh` documents the original multi-GPU training setup; it is not
a one-GPU Windows setup script. The full training/quality experiments have not
been rerun as part of the current Windows validation. Keep upstream licenses and
dataset/model terms. See the root README for the supported experiment.
