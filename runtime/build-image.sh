#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
  exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
exec python3 "$task_root/runtime/build-image.py" "$@"
