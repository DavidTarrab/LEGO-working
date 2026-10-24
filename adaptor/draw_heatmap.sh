#!/usr/bin/env bash
# Original research entrypoint; not part of the validated Windows experiment.
set -euo pipefail
: "${MODEL_PATH:?Set MODEL_PATH to an authorized model directory or model ID}"
: "${DATASET_PATH:?Set DATASET_PATH to a dataset directory or dataset ID}"
cd -- "$(dirname -- "$0")"
python struct_heatmap.py --model_path "$MODEL_PATH" --dataset_name "$DATASET_PATH" \
  --split train --batch_size "${BATCH_SIZE:-10}" --batch_num "${BATCH_NUM:-250}"
