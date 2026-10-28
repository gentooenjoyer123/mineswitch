/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "nx_jit.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <switch.h>

enum { ARENA_LIMIT = 32, CODE_PAGE = 4096 };
struct NxJitArena {
    Jit memory;
    uintptr_t rw;
    uintptr_t rx;
    size_t bytes;
    atomic_bool active;
};
static struct NxJitArena arenas[ARENA_LIMIT];
static Mutex arena_lock;
static _Thread_local uint32_t last_result;

static bool aligned_size(size_t bytes) { return bytes && !(bytes & (CODE_PAGE - 1)); }

NxJitArena *nxjit_create(size_t bytes, size_t alignment) {
    last_result = 0;
    if (!aligned_size(bytes) || alignment < CODE_PAGE || (alignment & (alignment - 1)) ||
        bytes > SIZE_MAX - (alignment - CODE_PAGE)) { errno = EINVAL; return NULL; }
    mutexLock(&arena_lock);
    struct NxJitArena *arena = NULL;
    for (size_t i = 0; i < ARENA_LIMIT; ++i) {
        if (!atomic_load_explicit(&arenas[i].active, memory_order_acquire)) { arena = &arenas[i]; break; }
    }
    if (!arena) { mutexUnlock(&arena_lock); errno = ENOSPC; return NULL; }
    size_t physical = bytes + alignment - CODE_PAGE;
    Result result = jitCreate(&arena->memory, physical);
    if (R_FAILED(result)) {
        last_result = result;
        mutexUnlock(&arena_lock); errno = ENOMEM; return NULL;
    }
    // Only the loader-advertised public CodeMemory route is used. Never fall
    // back to process-permission services or assume an RW address executes.
    if (arena->memory.type != JitType_CodeMemory) {
        result = jitClose(&arena->memory);
        if (R_FAILED(result)) diagAbortWithResult(result);
        mutexUnlock(&arena_lock); errno = ENOTSUP; return NULL;
    }
    uintptr_t raw_rw = (uintptr_t)jitGetRwAddr(&arena->memory);
    size_t offset = ((raw_rw + alignment - 1) & ~(uintptr_t)(alignment - 1)) - raw_rw;
    arena->rw = raw_rw + offset;
    arena->rx = (uintptr_t)jitGetRxAddr(&arena->memory) + offset;
    arena->bytes = bytes;
    memset((void*)arena->rw, 0, bytes);
    atomic_store_explicit(&arena->active, true, memory_order_release);
    mutexUnlock(&arena_lock);
    return arena;
}

void *nxjit_rw_address(const NxJitArena *arena) { return arena ? (void*)arena->rw : NULL; }
void *nxjit_rx_address(const NxJitArena *arena) { return arena ? (void*)arena->rx : NULL; }
size_t nxjit_size(const NxJitArena *arena) { return arena ? arena->bytes : 0; }
uint32_t nxjit_last_result(void) { return last_result; }

static struct NxJitArena *find_alias(const void *address, uintptr_t *base) {
    uintptr_t value = (uintptr_t)address;
    // An actual mapped address takes precedence over another arena's end bound.
    for (size_t i = 0; i < ARENA_LIMIT; ++i) {
        struct NxJitArena *arena = &arenas[i];
        if (!atomic_load_explicit(&arena->active, memory_order_acquire)) continue;
        if (value >= arena->rw && value - arena->rw < arena->bytes) {
            *base = arena->rw; return arena;
        }
        if (value >= arena->rx && value - arena->rx < arena->bytes) {
            *base = arena->rx; return arena;
        }
    }
    for (size_t i = 0; i < ARENA_LIMIT; ++i) {
        struct NxJitArena *arena = &arenas[i];
        if (!atomic_load_explicit(&arena->active, memory_order_acquire)) continue;
        if (value == arena->rw + arena->bytes) { *base = arena->rw; return arena; }
        if (value == arena->rx + arena->bytes) { *base = arena->rx; return arena; }
    }
    return NULL;
}

void *nxjit_writable_address(const void *address) {
    uintptr_t base;
    struct NxJitArena *arena = find_alias(address, &base);
    return arena ? (void*)(arena->rw + (uintptr_t)address - base) : (void*)address;
}
void *nxjit_executable_address(const void *address) {
    uintptr_t base;
    struct NxJitArena *arena = find_alias(address, &base);
    return arena ? (void*)(arena->rx + (uintptr_t)address - base) : (void*)address;
}
bool nxjit_contains(const void *address, size_t bytes) {
    uintptr_t base;
    struct NxJitArena *arena = find_alias(address, &base);
    return arena && bytes <= arena->bytes - ((uintptr_t)address - base);
}

void nxjit_flush_instruction_cache(void *address, size_t bytes) {
    if (!bytes) return;
    if (!address || bytes > UINTPTR_MAX - (uintptr_t)address)
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
    uintptr_t base;
    struct NxJitArena *arena = find_alias(address, &base);
    void *rw = address, *rx = address;
    if (arena) {
        size_t offset = (uintptr_t)address - base;
        if (bytes > arena->bytes - offset)
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));
        rw = (void*)(arena->rw + offset);
        rx = (void*)(arena->rx + offset);
    }
    // Ordinary temporary assembler buffers use their actual address for both
    // operations. CodeMemory buffers must clean RW and invalidate RX; flushing
    // only the address passed by the caller would miss one of the aliases.
    armDCacheFlush(rw, bytes);
    armICacheInvalidate(rx, bytes);
}

bool nxjit_destroy(NxJitArena *arena) {
    last_result = 0;
    uintptr_t value = (uintptr_t)arena, first = (uintptr_t)arenas;
    if (!arena || value < first || value - first >= sizeof(arenas) ||
        (value - first) % sizeof(*arena) != 0) { errno = EINVAL; return false; }
    mutexLock(&arena_lock);
    if (!atomic_load_explicit(&arena->active, memory_order_acquire)) {
        mutexUnlock(&arena_lock); errno = EINVAL; return false;
    }
    // Caller guarantees all executing/looking-up threads are quiescent.
    atomic_store_explicit(&arena->active, false, memory_order_release);
    Result result = jitClose(&arena->memory);
    if (R_FAILED(result)) {
        // jitClose can have partially removed mappings. Do not let the VM
        // continue using a fictitious mapping or return to a homebrew loader.
        diagAbortWithResult(result);
    }
    mutexUnlock(&arena_lock);
    return true;
}

bool nxjit_release_memory(void *rw_base, size_t bytes) {
    uintptr_t base;
    struct NxJitArena *arena = find_alias(rw_base, &base);
    if (!arena || (uintptr_t)rw_base != arena->rw || bytes != arena->bytes) {
        last_result = 0; errno = EINVAL; return false;
    }
    return nxjit_destroy(arena);
}
