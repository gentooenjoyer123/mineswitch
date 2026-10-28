#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
    exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
export MINESWITCH_GENERATED_DIR="$task_root/runtime/generated-native-minecraft"
export MINESWITCH_LAUNCHER_BUILD="$task_root/runtime/launcher/build-file-sharing"
export MINESWITCH_LAUNCHER_OUTPUT="$task_root/runtime/launcher/MineSwitch-FileSharing"
export MINESWITCH_LAUNCH_MODE=probe
mkdir -p "$MINESWITCH_GENERATED_DIR/romfs/tests" "$MINESWITCH_GENERATED_DIR/romfs/launch"
javac --release 21 -d "$MINESWITCH_GENERATED_DIR/romfs/tests" \
    "$task_root/runtime/launcher/java/mineswitch/probe/FileSharingProbe.java"
cat > "$MINESWITCH_GENERATED_DIR/romfs/launch/file-sharing.options" <<'OPTIONS'
-Xms32m
-Xmx128m
-XX:ReservedCodeCacheSize=64m
-XX:InitialCodeCacheSize=8m
-XX:-SegmentedCodeCache
-XX:CICompilerCount=2
OPTIONS
exec bash "$task_root/runtime/build-launcher.sh" "$@"
