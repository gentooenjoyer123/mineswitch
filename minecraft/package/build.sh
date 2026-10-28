#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
    exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
export DEVKITPRO=${DEVKITPRO:-"$task_root/.toolchain/opt/devkitpro"}
export DEVKITA64=${DEVKITA64:-"$DEVKITPRO/devkitA64"}
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
python3 "$task_root/minecraft/package/prepare.py" "$@"
exec make -C "$task_root/minecraft/package" -j6
