#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 "$task_root/runtime/limit-build.py" -- make -C "$task_root/launcher" -j6 "$@"
