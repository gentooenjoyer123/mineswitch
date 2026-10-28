#!/usr/bin/env bash
# Configure the real Horizon/newlib target; build tools run on the host JDK.
set -euo pipefail

task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
  exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
jdk_src=${MINESWITCH_JDK_SOURCE:-"$task_root/.upstream/openjdk21"}
export DEVKITPRO=${DEVKITPRO:-"$task_root/.toolchain/opt/devkitpro"}
export DEVKITA64=${DEVKITA64:-"$DEVKITPRO/devkitA64"}
boot_jdk=${MINESWITCH_BOOT_JDK:-/opt/openjdk-bin-21}
ffi_prefix=${MINESWITCH_LIBFFI:-"$task_root/.research/vm-probe-libffi/stage"}
jvm_variant=${MINESWITCH_JVM_VARIANT:-zero}
case "$jvm_variant" in
  zero)
    variant_options=(--with-libffi="$ffi_prefix")
    ;;
  server)
    variant_options=(--enable-jvm-feature-compiler1 --enable-jvm-feature-compiler2 \
      --disable-jvm-feature-jvmci)
    ;;
  *)
    printf 'Unsupported Horizon JVM variant: %s (use zero or server).\n' "$jvm_variant" >&2
    exit 2
    ;;
esac
conf_name=${MINESWITCH_JDK_CONF:-"horizon-aarch64-$jvm_variant-release"}

# The working Zero runtime is a separate artifact. Never repurpose an existing
# output tree for another backend, even when its configuration name is custom.
for option in "$@"; do
  case "$option" in
    --with-jvm-variants=*)
      [[ "$option" == "--with-jvm-variants=$jvm_variant" ]] || {
        printf 'Select the JVM with MINESWITCH_JVM_VARIANT, using a separate output.\n' >&2
        exit 2
      }
      ;;
    --with-conf-name=*)
      [[ "$option" == "--with-conf-name=$conf_name" ]] || {
        printf 'Select the output with MINESWITCH_JDK_CONF.\n' >&2
        exit 2
      }
      ;;
  esac
done
if [[ "$jvm_variant" == server && "$conf_name" == horizon-aarch64-zero-release ]]; then
  printf 'Refusing to replace the working Zero configuration with the server backend.\n' >&2
  exit 2
fi

# Configure and make both update shared state in an OpenJDK output tree.
# Keep parallel agents/processes from racing on generated files and logs.
mkdir -p -- "$jdk_src/build"
exec 9>"$jdk_src/build/.mineswitch-build.lock"
if ! flock -n 9; then
  printf 'Waiting for the current OpenJDK configure/build to finish.\n' >&2
  flock 9
fi

existing_spec="$jdk_src/build/$conf_name/spec.gmk"
if [[ -f "$existing_spec" ]]; then
  existing_variant=$(sed -n 's/^JVM_VARIANTS[[:space:]]*:=[[:space:]]*//p' "$existing_spec")
  if [[ -n "$existing_variant" && "$existing_variant" != "$jvm_variant" ]]; then
    printf 'Configuration %s already contains JVM variant %s; choose a new output.\n' \
      "$conf_name" "$existing_variant" >&2
    exit 2
  fi
fi

required_files=("$jdk_src/configure" "$boot_jdk/bin/java" \
  "$DEVKITA64/bin/aarch64-none-elf-g++")
if [[ "$jvm_variant" == zero ]]; then
  required_files+=("$ffi_prefix/lib/libffi.a")
fi
for required in "${required_files[@]}"; do
  if [[ ! -f "$required" ]]; then
    printf 'Missing prerequisite: %s\n' "$required" >&2
    exit 1
  fi
done

export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
target_flags="-march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -D__SWITCH__ -I$DEVKITPRO/libnx/include"
target_ldflags="-specs=$DEVKITPRO/libnx/switch.specs -L$DEVKITPRO/libnx/lib -lnx"

# OpenJDK's Boot JDK detector rejects the "Picked up _JAVA_OPTIONS" banner.
# Keep the inherited six-CPU affinity/nice limit, and express JVM options via
# configure's supported argument instead. Subsequent builds retain the normal
# limiter environment; only this configure process removes the banner sources.
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
  --with-jvm-variants="$jvm_variant" \
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
  "${variant_options[@]}" \
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
