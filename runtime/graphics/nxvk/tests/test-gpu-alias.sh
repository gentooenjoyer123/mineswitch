#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
if [[ ${MINESWITCH_BUILD_LIMITED:-0} != 1 ]]; then
    exec python3 "$task_root/runtime/limit-build.py" -- bash "$0" "$@"
fi
cd "$task_root"
test_build=$(mktemp -d)
trap 'rm -rf -- "$test_build"' EXIT
flags=(-std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror
       -fsanitize=address,undefined -pthread -DMINESWITCH_PRESSURE_HOST=1
       -Iruntime/launcher/tests/alloc-trace)
cc "${flags[@]}" -DMINESWITCH_MIMALLOC_HOST=1 -DMINESWITCH_GPU_ALIAS_HOST=1 \
    runtime/graphics/nxvk/tests/alias_host.c runtime/graphics/nxvk/gpu_alias.c \
    runtime/graphics/nxvk/cache_pressure.c runtime/memory/mimalloc/page_pool.c \
    -o "$test_build/alias"
"$test_build/alias"
cc "${flags[@]}" runtime/graphics/nxvk/tests/test-cache-pressure.c -o "$test_build/pressure"
"$test_build/pressure"
cc "${flags[@]}" -DMINESWITCH_NXVK_RECLAIM=1 \
    runtime/launcher/tests/alloc-trace/test.c runtime/graphics/nxvk/cache_pressure.c \
    -o "$test_build/realloc"
ASAN_OPTIONS=allocator_may_return_null=1 "$test_build/realloc"
