#!/usr/bin/env bash
set -euo pipefail
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
    exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
export DEVKITPRO="${DEVKITPRO:-${task_root}/.toolchain/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
export PATH="${DEVKITPRO}/tools/bin:${DEVKITA64}/bin:${PATH}"
allocator_backend=newlib
for argument in "$@"; do
    if [[ $argument == ALLOCATOR_BACKEND=* ]]; then allocator_backend=${argument#*=}; fi
done
case "$allocator_backend" in
    newlib|mimalloc) python3 "$task_root/runtime/memory/$allocator_backend/build.py" ;;
    *) echo "Unknown allocator backend: $allocator_backend" >&2; exit 1 ;;
esac
launcher_arguments=()
if [[ -n ${MINESWITCH_GENERATED_DIR:-} ]]; then
    launcher_arguments+=("REGISTRY=${MINESWITCH_GENERATED_DIR}/native_registry.c" "ROMFS=${MINESWITCH_GENERATED_DIR}/romfs")
fi
if [[ -n ${MINESWITCH_LAUNCHER_BUILD:-} ]]; then
    launcher_arguments+=("BUILD=${MINESWITCH_LAUNCHER_BUILD}")
fi
if [[ -n ${MINESWITCH_LAUNCHER_OUTPUT:-} ]]; then
    launcher_arguments+=("OUTPUT=${MINESWITCH_LAUNCHER_OUTPUT}")
fi
if [[ -n ${MINESWITCH_LAUNCH_MODE:-} ]]; then
    launcher_arguments+=("APP_MODE=${MINESWITCH_LAUNCH_MODE}")
fi
make -C "${task_root}/runtime/launcher" -j"${MINESWITCH_BUILD_JOBS:-6}" "${launcher_arguments[@]}" "$@"
