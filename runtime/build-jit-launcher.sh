#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export MINESWITCH_GENERATED_DIR="$task_root/runtime/generated-jit"
export MINESWITCH_LAUNCHER_BUILD="$task_root/runtime/launcher/build-jit"
export MINESWITCH_LAUNCHER_OUTPUT="$task_root/runtime/launcher/mineswitch-jit"
exec bash "$task_root/runtime/build-launcher.sh" "$@"
