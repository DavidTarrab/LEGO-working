#!/usr/bin/env bash
# Compatibility entrypoint: set MODEL_PATH to the chosen comparison model.
set -euo pipefail
exec bash "$(dirname -- "$0")/run_fighting.sh" "$@"
