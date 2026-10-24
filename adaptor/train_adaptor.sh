#!/usr/bin/env bash
# Original multi-GPU training entrypoint; install framework dependencies first.
set -euo pipefail
cd -- "$(dirname -- "$0")/LLaMA-Factory"
config="${TRAIN_CONFIG:-examples/train_full/llama3_full_sft.yaml}"
if [[ ! -f "$config" ]]; then
  echo "Training configuration not found: $config" >&2
  exit 1
fi
# Configure model/dataset paths in the selected YAML before launching.
export CUDA_VISIBLE_DEVICES="${CUDA_VISIBLE_DEVICES:-0,1,2,3}"
export FORCE_TORCHRUN="${FORCE_TORCHRUN:-1}"
llamafactory-cli train "$config"
