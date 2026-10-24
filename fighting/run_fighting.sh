#!/usr/bin/env bash
# Run once per terminal/agent; configure the opponent in the other terminal.
set -euo pipefail
: "${MODEL_PATH:?Set MODEL_PATH to an authorized model directory or model ID}"
cd -- "$(dirname -- "$0")"
export MODEL_PATH
export CUDA_VISIBLE_DEVICES="${CUDA_VISIBLE_DEVICES:-0}"
uvicorn ollama_server:app --host "${HOST:-127.0.0.1}" --port "${PORT:-11434}"
