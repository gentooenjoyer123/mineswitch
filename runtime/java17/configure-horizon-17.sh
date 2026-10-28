#!/usr/bin/env bash
# Configure OpenJDK 17u for the Horizon/newlib target (native AArch64 server VM).
# Mirrors runtime/configure-horizon.sh; the Java 21 scripts/trees are untouched.
set -euo pipefail

task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
  exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
jdk_src=${MINESWITCH_JDK17_SOURCE:-"$task_root/.upstream/openjdk17"}
export DEVKITPRO=${DEVKITPRO:-"$task_root/.toolchain/opt/devkitpro"}
export DEVKITA64=${DEVKITA64:-"$DEVKITPRO/devkitA64"}
# JDK 17 accepts a Boot JDK 16/17; the build JDK must be the same feature release.
boot_jdk=${MINESWITCH_BOOT_JDK17:-/opt/openjdk-bin-17}
conf_name=${MINESWITCH_JDK17_CONF:-horizon-aarch64-server-release}

mkdir -p -- "$jdk_src/build"
exec 9>"$jdk_src/build/.mineswitch-build.lock"
if ! flock -n 9; then
  printf 'Waiting for the current OpenJDK 17 configure/build to finish.\n' >&2
  flock 9
fi

for required in "$jdk_src/configure" "$boot_jdk/bin/java" "$DEVKITA64/bin/aarch64-none-elf-g++"; do
  if [[ ! -f "$required" ]]; then
    printf 'Missing prerequisite: %s\n' "$required" >&2
    exit 1
  fi
done
boot_version=$("$boot_jdk/bin/java" -version 2>&1 | sed -n 's/.*version "\([0-9]*\).*/\1/p' | head -n 1)
if [[ "$boot_version" != 17 ]]; then
  printf 'Boot/build JDK must be 17 (found %s at %s).\n' "$boot_version" "$boot_jdk" >&2
  exit 1
fi

export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
target_flags="-march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -D__SWITCH__ -I$DEVKITPRO/libnx/include"
target_ldflags="-specs=$DEVKITPRO/libnx/switch.specs -L$DEVKITPRO/libnx/lib -lnx"

boot_jvm_options="${JAVA_TOOL_OPTIONS:-} ${_JAVA_OPTIONS:--XX:ActiveProcessorCount=6}"
unset JAVA_TOOL_OPTIONS _JAVA_OPTIONS

cd -- "$jdk_src"
exec bash configure \
  --openjdk-target=aarch64-unknown-horizon \
  --with-conf-name="$conf_name" \
  --with-num-cores=6 \
  --with-jobs=6 \
  --with-toolchain-type=gcc \
  --with-boot-jdk="$boot_jdk" \
  --with-boot-jdk-jvmargs="$boot_jvm_options" \
  --with-build-jdk="$boot_jdk" \
  --with-jvm-variants=server \
  --with-debug-level=release \
  --enable-static-build \
  --enable-headless-only \
  --disable-precompiled-headers \
  --disable-warnings-as-errors \
  --disable-jvm-feature-cds \
  --disable-jvm-feature-jfr \
  --disable-jvm-feature-jvmti \
  --disable-jvm-feature-services \
  --enable-jvm-feature-management \
  --disable-jvm-feature-vm-structs \
  --disable-jvm-feature-g1gc \
  --disable-jvm-feature-parallelgc \
  --disable-jvm-feature-shenandoahgc \
  --disable-jvm-feature-epsilongc \
  --disable-jvm-feature-zgc \
  --disable-jvm-feature-jvmci \
  --enable-jvm-feature-compiler1 \
  --enable-jvm-feature-compiler2 \
  --with-native-debug-symbols=internal \
  --with-extra-cflags="$target_flags" \
  --with-extra-cxxflags="$target_flags" \
  --with-extra-ldflags="$target_ldflags" \
  CC="$DEVKITA64/bin/aarch64-none-elf-gcc" \
  CXX="$DEVKITA64/bin/aarch64-none-elf-g++" \
  AR="$DEVKITA64/bin/aarch64-none-elf-ar" \
  NM="$DEVKITA64/bin/aarch64-none-elf-nm" \
  STRIP="$DEVKITA64/bin/aarch64-none-elf-strip" \
  OBJCOPY="$DEVKITA64/bin/aarch64-none-elf-objcopy" \
  OBJDUMP="$DEVKITA64/bin/aarch64-none-elf-objdump" \
  "$@"
