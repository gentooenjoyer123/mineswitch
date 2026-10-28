#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export MINESWITCH_JVM_VARIANT=server
export MINESWITCH_JDK_CONF=horizon-aarch64-server-release
export MINESWITCH_GENERATED_DIR="$task_root/runtime/generated-jit"
# Start with the modules needed to validate real generated Java execution.
export MINESWITCH_MODULES="${MINESWITCH_MODULES:-java.base,java.management,jdk.management}"
export MINESWITCH_NATIVE_SPECS="${MINESWITCH_NATIVE_SPECS:-none}"
exec bash "$task_root/runtime/build-image.sh" "$@"
