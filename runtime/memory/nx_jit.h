/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#ifndef MINESWITCH_NX_JIT_H
#define MINESWITCH_NX_JIT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NxJitArena NxJitArena;

/* Eager physical backing with distinct simultaneous RW and RX CodeMemory
 * aliases. Allocation/release are serialized. Release requires caller-proven
 * quiescence: no execution or lookup may still reference the arena. */
NxJitArena *nxjit_create(size_t bytes, size_t alignment);
void *nxjit_rw_address(const NxJitArena *arena);
void *nxjit_rx_address(const NxJitArena *arena);
size_t nxjit_size(const NxJitArena *arena);
bool nxjit_destroy(NxJitArena *arena);
/* Releases only an exact full RW arena after caller-proven quiescence. */
bool nxjit_release_memory(void *rw_base, size_t bytes);
uint32_t nxjit_last_result(void);

/* Canonical translation preserves offsets. Already appropriate aliases and
 * ordinary temporary buffers are unchanged. One-past-end bounds translate too.
 * Alignment applies to the RW allocation; RX keeps page/code-entry alignment.
 * These lookups must remain lock-free for runtime/exception publication paths. */
void *nxjit_writable_address(const void *address);
void *nxjit_executable_address(const void *address);
bool nxjit_contains(const void *address, size_t bytes);
void nxjit_flush_instruction_cache(void *address, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
