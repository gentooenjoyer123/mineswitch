#!/usr/bin/env bash
set -euo pipefail

task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
    exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
export DEVKITPRO="${DEVKITPRO:-${task_root}/.toolchain/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
export PATH="${DEVKITPRO}/tools/bin:${DEVKITA64}/bin:${PATH}"

if [[ ! -f "${DEVKITPRO}/portlibs/switch/lib/libEGL.a" ]]; then
    echo 'Switch graphics libraries are missing; run scripts/bootstrap-toolchain.py.' >&2
    exit 1
fi

exec make -C "${task_root}/graphics" "$@"
