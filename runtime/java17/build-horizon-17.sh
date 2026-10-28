#!/usr/bin/env bash
# Build targets of the OpenJDK 17 Horizon configuration (default: hotspot).
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
  exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
jdk_src=${MINESWITCH_JDK17_SOURCE:-"$task_root/.upstream/openjdk17"}
export DEVKITPRO=${DEVKITPRO:-"$task_root/.toolchain/opt/devkitpro"}
export DEVKITA64=${DEVKITA64:-"$DEVKITPRO/devkitA64"}
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
conf_name=${MINESWITCH_JDK17_CONF:-horizon-aarch64-server-release}
mkdir -p -- "$jdk_src/build"
exec 9>"$jdk_src/build/.mineswitch-build.lock"
if ! flock -n 9; then
  printf 'Waiting for the current OpenJDK 17 configure/build to finish.\n' >&2
  flock 9
fi
if [[ ! -f "$jdk_src/build/$conf_name/spec.gmk" ]]; then
  printf 'Run runtime/java17/configure-horizon-17.sh first.\n' >&2
  exit 1
fi
cd -- "$jdk_src"
if [[ $# -eq 0 ]]; then
  set -- hotspot
fi
exec make CONF="$conf_name" JOBS="${MINESWITCH_JOBS:-6}" \
  HORIZON_RUNTIME_DIR="$task_root/runtime" "$@"
