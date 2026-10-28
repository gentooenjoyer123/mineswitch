#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export MINESWITCH_GENERATED_DIR="$task_root/runtime/generated-native-minecraft"
export MINESWITCH_LAUNCHER_BUILD="$task_root/runtime/launcher/build-standalone"
export MINESWITCH_LAUNCHER_OUTPUT="$task_root/runtime/launcher/MineSwitch"
export MINESWITCH_LAUNCH_MODE=minecraft
export MINESWITCH_STANDALONE=1
python3 "$task_root/minecraft/prepare-launch.py"
exec bash "$task_root/runtime/build-launcher.sh" "$@"
