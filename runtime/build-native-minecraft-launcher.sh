#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export MINESWITCH_GENERATED_DIR="$task_root/runtime/generated-native-minecraft"
export MINESWITCH_LAUNCHER_BUILD="$task_root/runtime/launcher/build-native-minecraft"
export MINESWITCH_LAUNCHER_OUTPUT="$task_root/runtime/launcher/mineswitch-native-minecraft"
exec bash "$task_root/runtime/build-launcher.sh" "$@"
