#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
    exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
export MINESWITCH_GENERATED_DIR="$task_root/runtime/generated-native-minecraft"
export MINESWITCH_LAUNCHER_BUILD="$task_root/runtime/launcher/build-nxvk"
export MINESWITCH_LAUNCHER_OUTPUT="$task_root/runtime/launcher/MineSwitch-NXVK"
export MINESWITCH_LAUNCH_MODE=launcher
python3 "$task_root/minecraft/prepare-launch.py"
exec bash "$task_root/runtime/build-launcher.sh" GRAPHICS_BACKEND=nxvk "$@"
