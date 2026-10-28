/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#ifndef MINESWITCH_NX_MEMORY_H
#define MINESWITCH_NX_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NXVM_PAGE_SIZE = 4096 };
enum nxvm_permission { NXVM_NONE = 0, NXVM_READ = 1, NXVM_READ_WRITE = 3 };
struct nxvm_memory_stats {
    size_t reservations;
    size_t reserved_bytes;
    size_t committed_bytes;
    size_t backing_bytes;
};

/*
 * All ranges must have page-aligned base and positive page-multiple size.
 * Reserves have eager physical backing from newlib, initially protected NONE.
 * Uncommit revokes access; it does NOT return backing to the kernel/newlib.
 * Recommit zeroes previously uncommitted pages. Committed pages retain data.
 * Partial release leaves inaccessible holes until the entire allocation dies.
 * No executable or fixed-address allocation is provided by this backend.
 */
void *nxvm_reserve(size_t bytes, size_t alignment, bool executable);
bool nxvm_commit(void *base, size_t bytes, bool executable);
bool nxvm_uncommit(void *base, size_t bytes);
bool nxvm_release(void *base, size_t bytes);
bool nxvm_protect(void *base, size_t bytes, enum nxvm_permission permission);
/* Prepare the current SDK-created thread's owned stack mapping and reserve its
 * VA before enabling guards. This may allocate/lock and must precede faults.
 * The loader main stack has no owned source pair and returns ENOTSUP. */
bool nxvm_stack_guard_prepare(void);
/* Uses public unmap/remap on the prepared owned mirror/backing pair. Supports
 * NONE and RW; transitions allocate nothing and acquire no allocator lock. */
bool nxvm_protect_current_stack(void *base, size_t bytes, enum nxvm_permission permission);
/* Restore every owned hole and release tracking before SDK threadClose. Must
 * run outside exception dispatch on the owner thread. Idempotent if unprepared. */
bool nxvm_stack_guard_cleanup(void);
bool nxvm_discard(void *base, size_t bytes);
/* Named immutable-file snapshot; opens a private descriptor (newlib lacks pread). */
void *nxvm_map_file_private(const char *path, uint64_t offset, size_t bytes, bool read_only);
bool nxvm_unmap_file(void *base, size_t bytes);
void nxvm_get_memory_stats(struct nxvm_memory_stats *stats);
/* Current thread's last libnx failure (zero for validation/allocator failures). */
uint32_t nxvm_last_result(void);

#ifdef __cplusplus
}
#endif
#endif
