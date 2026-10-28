#!/usr/bin/env bash
set -euo pipefail
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
  exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
export DEVKITPRO="${DEVKITPRO:-${task_root}/.toolchain/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
source_root="${task_root}/.upstream/glfw-switch"
build_root="${task_root}/runtime/glfw/build"

cmake -S "${source_root}" -B "${build_root}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${task_root}/runtime/glfw/toolchain.cmake" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF \
  -DGLFW_BUILD_DOCS=OFF -DGLFW_INSTALL=OFF \
  -DGLFW_EGL_STATIC=ON -DGLFW_VULKAN_STATIC=OFF
cmake --build "${build_root}" --parallel "${MINESWITCH_BUILD_JOBS:-6}"
cp -- "${build_root}/src/libglfw3.a" "${build_root}/libglfw3.a"
