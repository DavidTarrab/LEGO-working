#!/usr/bin/env bash
# Install lm-evaluation-harness separately in the desired Python environment.
set -euo pipefail
: "${MODEL_PATH:?Set MODEL_PATH to the model to evaluate}"
lm_eval --model hf --model_args "pretrained=${MODEL_PATH},trust_remote_code=${TRUST_REMOTE_CODE:-False}" \
  --tasks "${TASKS:-mmlu,squadv2,arc_challenge_chat}" \
  --device "${DEVICE:-cuda:0}" --batch_size "${BATCH_SIZE:-8}"
