#!/usr/bin/env bash
# Cross-compile the JDK 8u HotSpot (server: C1+C2 tiered, Serial GC) for the
# Horizon/newlib target into a static libjvm.a. Standalone HotSpot make (no
# JDK 8 configure); OSNAME=horizon selects hotspot/make/horizon.
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
  exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
jdk_src=${MINESWITCH_JDK8_SOURCE:-"$task_root/.upstream/openjdk8"}
out=${MINESWITCH_JDK8_HOTSPOT_OUT:-"$jdk_src/build/horizon-aarch64-hotspot"}
export DEVKITPRO=${DEVKITPRO:-"$task_root/.toolchain/opt/devkitpro"}
export DEVKITA64=${DEVKITA64:-"$DEVKITPRO/devkitA64"}
boot_jdk=${MINESWITCH_BOOT_JDK8:-/opt/openjdk-bin-8}
tool="$DEVKITA64/bin/aarch64-none-elf"
mkdir -p -- "$out"
exec 9>"$jdk_src/.mineswitch-build.lock"
if ! flock -n 9; then
  printf 'Waiting for the current OpenJDK 8 build to finish.\n' >&2
  flock 9
fi
unset JAVA_TOOL_OPTIONS
cd -- "$jdk_src/hotspot/make"
if [[ $# -eq 0 ]]; then set -- product; fi
exec make OSNAME=horizon ARCH=aarch64 BUILDARCH=aarch64 LIBARCH=aarch64 SRCARCH=aarch64 \
  ARCH_DATA_MODEL=64 LP64=1 CROSS_COMPILE_ARCH=aarch64 \
  CC="$tool-gcc" CXX="$tool-g++" HOSTCC=gcc HOSTCXX=g++ \
  AR="$tool-ar" NM="$tool-nm" STRIP="$tool-strip" OBJCOPY="$tool-objcopy" \
  ALT_BOOTDIR="$boot_jdk" ALT_OUTPUTDIR="$out" ALT_EXPORT_PATH="$out/export" \
  HORIZON_RUNTIME_DIR="$task_root/runtime" HORIZON_DEVKITPRO="$DEVKITPRO" \
  INCLUDE_JVMTI=false INCLUDE_FPROF=false INCLUDE_VM_STRUCTS=false \
  INCLUDE_SERVICES=false INCLUDE_CDS=false \
  ENABLE_JFR=false INCLUDE_JFR=0 \
  USE_PRECOMPILED_HEADER=0 ENABLE_FULL_DEBUG_SYMBOLS=0 ZIP_DEBUGINFO_FILES=0 \
  HOTSPOT_BUILD_JOBS="${MINESWITCH_JOBS:-6}" \
  HOTSPOT_BUILD_VERSION=mineswitch-horizon JRE_RELEASE_VERSION=1.8.0_492 \
  "$@"
