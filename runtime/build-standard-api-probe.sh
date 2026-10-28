#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
    exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
export MINESWITCH_GENERATED_DIR="$task_root/runtime/generated-native-minecraft"
export MINESWITCH_LAUNCHER_BUILD="$task_root/runtime/launcher/build-standard-api"
export MINESWITCH_LAUNCHER_OUTPUT="$task_root/runtime/launcher/MineSwitch-StandardApi"
export MINESWITCH_LAUNCH_MODE=probe
python3 "$task_root/minecraft/prepare-launch.py"
mkdir -p "$MINESWITCH_GENERATED_DIR/romfs/tests"
javac --release 21 -d "$MINESWITCH_GENERATED_DIR/romfs/tests" \
    "$task_root/runtime/launcher/java/mineswitch/probe/StandardApiProbe.java"
python3 - "$task_root" <<'PY'
import json, pathlib, sys
root=pathlib.Path(sys.argv[1])
policy=json.loads((root/'minecraft/module-policy.json').read_text())
options=['-Xms32m','-Xmx128m','-XX:ReservedCodeCacheSize=64m','-XX:InitialCodeCacheSize=8m',
         '-XX:-SegmentedCodeCache','-XX:CICompilerCount=2','-Djava.net.preferIPv4Stack=true',*policy['vm_options']]
(root/'runtime/generated-native-minecraft/romfs/launch/standard-api.options').write_text('\n'.join(options)+'\n')
PY
exec bash "$task_root/runtime/build-launcher.sh" "$@"
