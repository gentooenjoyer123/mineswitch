/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "nx_memory.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

/* Page state includes actual protection, not merely desired VM protection. */
enum { PAGE_RESERVED = 0x10, PAGE_COMMITTED = 0x20, PAGE_RELEASED = 0x40,
       PAGE_PERMISSION = 3 };
struct reservation {
    struct reservation *next;
    unsigned char *base;
    size_t size;
    size_t live_pages;
    unsigned char pages[];
};
static Mutex memory_lock;
static struct reservation *reservations;
static _Thread_local uint32_t last_result;

static void reserve_failure(const char *stage, size_t bytes, size_t alignment) {
    int saved_errno = errno;
    struct mallinfo info = mallinfo();
    extern char *fake_heap_end;
    void *brk = sbrk(0);
    size_t unclaimed = brk != (void *)-1 && (uintptr_t)brk < (uintptr_t)fake_heap_end
        ? (uintptr_t)fake_heap_end - (uintptr_t)brk : 0;
    fprintf(stderr, "MINESWITCH_RESERVE_FAILED stage=%s bytes=%zu alignment=%zu "
        "errno=%d sdk_rc=0x%08x arena=%zu allocated=%zu free_arena=%zu top=%zu unclaimed=%zu\n",
        stage, bytes, alignment, saved_errno, last_result, info.arena, info.uordblks,
        info.fordblks, info.keepcost, unclaimed);
    errno = saved_errno;
}

static bool fail(int error) {
    errno = error;
    return false;
}

static bool valid_range(const void *base, size_t bytes) {
    return base && bytes && !((uintptr_t)base & (NXVM_PAGE_SIZE - 1)) &&
        !(bytes & (NXVM_PAGE_SIZE - 1)) && (uintptr_t)base <= UINTPTR_MAX - bytes;
}

/* Caller holds memory_lock. A range may not span allocations or released holes. */
static struct reservation *find_range(void *base, size_t bytes, size_t *first) {
    if (!valid_range(base, bytes)) {
        fail(EINVAL);
        return NULL;
    }
    uintptr_t start = (uintptr_t)base;
    for (struct reservation *r = reservations; r; r = r->next) {
        if (start < (uintptr_t)r->base || bytes > r->size ||
            start - (uintptr_t)r->base > r->size - bytes) continue;
        *first = (start - (uintptr_t)r->base) / NXVM_PAGE_SIZE;
        size_t end = *first + bytes / NXVM_PAGE_SIZE;
        for (size_t i = *first; i < end; ++i) {
            if (r->pages[i] & PAGE_RELEASED) {
                fail(EINVAL);
                return NULL;
            }
        }
        return r;
    }
    fail(EINVAL);
    return NULL;
}

/*
 * svcSetMemoryPermission requires a uniform OLD permission across its range.
 * Split at old permission boundaries; retain the page map until all calls pass
 * so that a later failure can restore earlier runs. Failed rollback must stop
 * the process: continuing with inconsistent protection would corrupt the VM.
 */
static bool set_permissions(struct reservation *r, size_t first, size_t count,
                            unsigned permission) {
    size_t end = first + count;
    size_t changed_end = first;
    while (changed_end < end) {
        unsigned old = r->pages[changed_end] & PAGE_PERMISSION;
        size_t run_end = changed_end + 1;
        while (run_end < end && (r->pages[run_end] & PAGE_PERMISSION) == old) ++run_end;
        if (old != permission) {
            Result result = svcSetMemoryPermission(r->base + changed_end * NXVM_PAGE_SIZE,
                (run_end - changed_end) * NXVM_PAGE_SIZE, permission);
            if (R_FAILED(result)) {
                last_result = result;
                for (size_t undo = first; undo < changed_end;) {
                    unsigned restore = r->pages[undo] & PAGE_PERMISSION;
                    size_t undo_end = undo + 1;
                    while (undo_end < changed_end &&
                           (r->pages[undo_end] & PAGE_PERMISSION) == restore) ++undo_end;
                    Result rollback = svcSetMemoryPermission(r->base + undo * NXVM_PAGE_SIZE,
                        (undo_end - undo) * NXVM_PAGE_SIZE, restore);
                    if (R_FAILED(rollback)) diagAbortWithResult(rollback);
                    undo = undo_end;
                }
                return fail(EACCES);
            }
        }
        changed_end = run_end;
    }
    for (size_t i = first; i < end; ++i)
        r->pages[i] = (r->pages[i] & ~PAGE_PERMISSION) | permission;
    return true;
}

void *nxvm_reserve(size_t bytes, size_t alignment, bool executable) {
    last_result = 0;
    if (executable) { fail(ENOTSUP); return NULL; }
    if (!bytes || (bytes & (NXVM_PAGE_SIZE - 1)) || alignment < NXVM_PAGE_SIZE ||
        (alignment & (alignment - 1))) { fail(EINVAL); return NULL; }
    size_t count = bytes / NXVM_PAGE_SIZE;
    if (count > SIZE_MAX - sizeof(struct reservation)) { fail(ENOMEM); return NULL; }
    struct reservation *r = malloc(sizeof(*r) + count);
    if (!r) { reserve_failure("metadata", bytes, alignment); return NULL; }
    r->base = memalign(alignment, bytes);
    if (!r->base) { reserve_failure("backing", bytes, alignment); free(r); return NULL; }
    r->size = bytes;
    r->live_pages = count;
    memset(r->pages, PAGE_RESERVED | NXVM_READ_WRITE, count);
    /* Still private to this call, so no reservation-list lock is needed. */
    if (!set_permissions(r, 0, count, NXVM_NONE)) {
        reserve_failure("permission", bytes, alignment);
        free(r->base);
        free(r);
        return NULL;
    }
    mutexLock(&memory_lock);
    r->next = reservations;
    reservations = r;
    mutexUnlock(&memory_lock);
    return r->base;
}

bool nxvm_commit(void *base, size_t bytes, bool executable) {
    last_result = 0;
    if (executable) return fail(ENOTSUP);
    mutexLock(&memory_lock);
    size_t first;
    struct reservation *r = find_range(base, bytes, &first);
    bool success = r && set_permissions(r, first, bytes / NXVM_PAGE_SIZE, NXVM_READ_WRITE);
    if (success) {
        size_t end = first + bytes / NXVM_PAGE_SIZE;
        for (size_t i = first; i < end;) {
            if (r->pages[i] & PAGE_COMMITTED) { ++i; continue; }
            size_t run_end = i + 1;
            while (run_end < end && !(r->pages[run_end] & PAGE_COMMITTED)) ++run_end;
            memset(r->base + i * NXVM_PAGE_SIZE, 0, (run_end - i) * NXVM_PAGE_SIZE);
            memset(r->pages + i, PAGE_COMMITTED | NXVM_READ_WRITE, run_end - i);
            i = run_end;
        }
    }
    mutexUnlock(&memory_lock);
    return success;
}

bool nxvm_uncommit(void *base, size_t bytes) {
    last_result = 0;
    mutexLock(&memory_lock);
    size_t first;
    struct reservation *r = find_range(base, bytes, &first);
    bool success = r && set_permissions(r, first, bytes / NXVM_PAGE_SIZE, NXVM_NONE);
    if (success) memset(r->pages + first, PAGE_RESERVED | NXVM_NONE, bytes / NXVM_PAGE_SIZE);
    mutexUnlock(&memory_lock);
    return success;
}

bool nxvm_release(void *base, size_t bytes) {
    last_result = 0;
    mutexLock(&memory_lock);
    size_t first;
    struct reservation *r = find_range(base, bytes, &first);
    bool success = false;
    if (r) {
        size_t count = bytes / NXVM_PAGE_SIZE;
        if (count == r->live_pages) {
            /* Restore every page, including prior holes, before malloc sees it. */
            success = set_permissions(r, 0, r->size / NXVM_PAGE_SIZE, NXVM_READ_WRITE);
            if (success) {
                struct reservation **link = &reservations;
                while (*link != r) link = &(*link)->next;
                *link = r->next;
                free(r->base);
                free(r);
            }
        } else {
            success = set_permissions(r, first, count, NXVM_NONE);
            if (success) {
                memset(r->pages + first, PAGE_RELEASED | NXVM_NONE, count);
                r->live_pages -= count;
            }
        }
    }
    mutexUnlock(&memory_lock);
    return success;
}

bool nxvm_protect(void *base, size_t bytes, enum nxvm_permission permission) {
    last_result = 0;
    if (permission != NXVM_NONE && permission != NXVM_READ && permission != NXVM_READ_WRITE)
        return fail(ENOTSUP);
    mutexLock(&memory_lock);
    size_t first;
    struct reservation *r = find_range(base, bytes, &first);
    bool success = r != NULL;
    if (r && permission != NXVM_NONE) {
        for (size_t i = first; i < first + bytes / NXVM_PAGE_SIZE; ++i) {
            if (!(r->pages[i] & PAGE_COMMITTED)) { success = fail(EINVAL); break; }
        }
    }
    if (success) success = set_permissions(r, first, bytes / NXVM_PAGE_SIZE, permission);
    mutexUnlock(&memory_lock);
    return success;
}

/* Stack mappings (svcMapMemory destinations) reject permission changes.
 * Keep their original SDK source/destination pair and create real guard holes
 * with public unmap/map operations. Everything required by the fault path is
 * allocated before the first guard; transitions take no allocator mutex. */
struct stack_guard_record {
    Thread *owner;
    unsigned char *mirror;
    unsigned char *backing;
    size_t stack_bytes;
    size_t page_count;
    VirtmemReservation *virtual_reservation;
    unsigned char guarded[];
};
static _Thread_local struct stack_guard_record *stack_guards;

static bool stack_pair_page_valid(const struct stack_guard_record *record, size_t page) {
    MemoryInfo destination, source;
    u32 page_info;
    uintptr_t offset = page * NXVM_PAGE_SIZE;
    Result result = svcQueryMemory(&destination, &page_info, (uintptr_t)record->mirror + offset);
    if (R_FAILED(result)) { last_result = result; return fail(EACCES); }
    result = svcQueryMemory(&source, &page_info, (uintptr_t)record->backing + offset);
    if (R_FAILED(result)) { last_result = result; return fail(EACCES); }
    if (record->guarded[page]) {
        if (destination.type != MemType_Unmapped || source.type != MemType_Heap ||
            source.perm != Perm_Rw || (source.attr & MemAttr_IsBorrowed)) return fail(EINVAL);
    } else {
        if (destination.type != MemType_MappedMemory || destination.perm != Perm_Rw ||
            source.type != MemType_Heap || source.perm != Perm_None ||
            !(source.attr & MemAttr_IsBorrowed)) return fail(EINVAL);
    }
    return true;
}

bool nxvm_stack_guard_prepare(void) {
    last_result = 0;
    Thread *self = threadGetSelf();
    if (stack_guards) return stack_guards->owner == self ? true : fail(EINVAL);
    // The loader stack does not expose an owned backing pair. Never infer one.
    if (!self || !self->stack_mem || !self->stack_mirror ||
        ((uintptr_t)self->stack_mem & (NXVM_PAGE_SIZE - 1)) ||
        ((uintptr_t)self->stack_mirror & (NXVM_PAGE_SIZE - 1))) return fail(ENOTSUP);
    size_t pages = self->stack_sz / NXVM_PAGE_SIZE;
    if (!pages || pages > SIZE_MAX - sizeof(struct stack_guard_record)) return fail(EINVAL);
    struct stack_guard_record *record = calloc(1, sizeof(*record) + pages);
    if (!record) return false;
    record->owner = self;
    record->mirror = self->stack_mirror;
    record->backing = self->stack_mem;
    record->stack_bytes = self->stack_sz;
    record->page_count = pages;
    // Check the recorded ABI pair before reserving virtual address space. Every
    // modified page is checked again at transition time.
    if (!stack_pair_page_valid(record, 0) || !stack_pair_page_valid(record, pages - 1)) {
        free(record);
        return false;
    }
    virtmemLock();
    record->virtual_reservation = virtmemAddReservation(record->mirror, pages * NXVM_PAGE_SIZE);
    virtmemUnlock();
    if (!record->virtual_reservation) { free(record); return fail(ENOMEM); }
    stack_guards = record;
    return true;
}

static Result stack_map_change(struct stack_guard_record *record, size_t page,
                               size_t count, bool guard) {
    void *mirror = record->mirror + page * NXVM_PAGE_SIZE;
    void *backing = record->backing + page * NXVM_PAGE_SIZE;
    size_t bytes = count * NXVM_PAGE_SIZE;
    return guard ? svcUnmapMemory(mirror, backing, bytes) : svcMapMemory(mirror, backing, bytes);
}

static bool stack_transition(struct stack_guard_record *record, size_t first,
                             size_t count, bool guard) {
    size_t end = first + count;
    for (size_t page = first; page < end; ++page)
        if (!stack_pair_page_valid(record, page)) return false;
    size_t changed_end = first;
    while (changed_end < end) {
        bool old_guard = record->guarded[changed_end] != 0;
        size_t run_end = changed_end + 1;
        while (run_end < end && (record->guarded[run_end] != 0) == old_guard) ++run_end;
        if (old_guard != guard) {
            Result result = stack_map_change(record, changed_end, run_end - changed_end, guard);
            if (R_FAILED(result)) {
                last_result = result;
                // The original bitmap is retained until the transaction ends.
                // It is also the rollback plan for all completed runs.
                for (size_t undo = first; undo < changed_end;) {
                    bool restore_guard = record->guarded[undo] != 0;
                    size_t undo_end = undo + 1;
                    while (undo_end < changed_end && (record->guarded[undo_end] != 0) == restore_guard) ++undo_end;
                    if (restore_guard != guard) {
                        Result rollback = stack_map_change(record, undo, undo_end - undo, restore_guard);
                        if (R_FAILED(rollback)) diagAbortWithResult(rollback);
                    }
                    undo = undo_end;
                }
                return fail(EACCES);
            }
        }
        changed_end = run_end;
    }
    memset(record->guarded + first, guard ? 1 : 0, count);
    return true;
}

bool nxvm_protect_current_stack(void *base, size_t bytes, enum nxvm_permission permission) {
    last_result = 0;
    if (!valid_range(base, bytes)) return fail(EINVAL);
    // MapMemory restores RW; it has no read-only stack mapping operation.
    if (permission != NXVM_NONE && permission != NXVM_READ_WRITE) return fail(ENOTSUP);
    Thread *self = threadGetSelf();
    struct stack_guard_record *record = stack_guards;
    if (!record || record->owner != self) return fail(ENOTSUP);
    uintptr_t address = (uintptr_t)base, bottom = (uintptr_t)record->mirror;
    if (self->stack_mirror != record->mirror || self->stack_mem != record->backing ||
        self->stack_sz != record->stack_bytes || bytes > record->stack_bytes ||
        address < bottom || address - bottom > record->stack_bytes - bytes) return fail(EINVAL);
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (permission == NXVM_NONE && sp >= bottom && sp - bottom < record->stack_bytes) {
        if (sp < 0x4000 || address + bytes > sp - 0x4000) return fail(EINVAL);
    }
    return stack_transition(record, (address - bottom) / NXVM_PAGE_SIZE,
                            bytes / NXVM_PAGE_SIZE, permission == NXVM_NONE);
}

bool nxvm_stack_guard_cleanup(void) {
    last_result = 0;
    struct stack_guard_record *record = stack_guards;
    if (!record) return true;
    if (record->owner != threadGetSelf()) return fail(EINVAL);
    // Restore any remaining holes before SDK threadClose unmaps the whole
    // original allocation. Cleanup is never called from the exception handler.
    for (size_t first = 0; first < record->page_count;) {
        if (!record->guarded[first]) { ++first; continue; }
        size_t end = first + 1;
        while (end < record->page_count && record->guarded[end]) ++end;
        if (!stack_transition(record, first, end - first, false)) return false;
        first = end;
    }
    virtmemLock();
    virtmemRemoveReservation(record->virtual_reservation);
    virtmemUnlock();
    stack_guards = NULL;
    free(record);
    return true;
}

bool nxvm_discard(void *base, size_t bytes) {
    last_result = 0;
    mutexLock(&memory_lock);
    size_t first;
    struct reservation *r = find_range(base, bytes, &first);
    bool success = r != NULL;
    if (r) {
        for (size_t i = first; i < first + bytes / NXVM_PAGE_SIZE; ++i) {
            if (r->pages[i] != (PAGE_COMMITTED | NXVM_READ_WRITE)) {
                success = fail(EINVAL); break;
            }
        }
    }
    if (success) memset(base, 0, bytes);
    mutexUnlock(&memory_lock);
    return success;
}

void *nxvm_map_file_private(const char *path, uint64_t offset, size_t bytes, bool read_only) {
    last_result = 0;
    if (!path || !bytes || bytes > SIZE_MAX - (NXVM_PAGE_SIZE - 1) ||
        offset > INT64_MAX || bytes > INT64_MAX - offset) { fail(EINVAL); return NULL; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    size_t rounded = (bytes + NXVM_PAGE_SIZE - 1) & ~(size_t)(NXVM_PAGE_SIZE - 1);
    void *base = nxvm_reserve(rounded, NXVM_PAGE_SIZE, false);
    if (!base) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    if (!nxvm_commit(base, rounded, false)) goto error;
    for (size_t done = 0; done < bytes;) {
        size_t count = bytes - done;
        if (count > INT_MAX) count = INT_MAX;
        ssize_t got = read(fd, (char *)base + done, count);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { if (!got) errno = EIO; goto error; }
        done += (size_t)got;
    }
    if (read_only && !nxvm_protect(base, rounded, NXVM_READ)) goto error;
    close(fd);
    return base;
error: {
    int saved_errno = errno;
    uint32_t saved_result = last_result;
    close(fd);
    if (!nxvm_release(base, rounded)) diagAbortWithResult(last_result);
    errno = saved_errno;
    last_result = saved_result;
    return NULL;
    }
}

bool nxvm_unmap_file(void *base, size_t bytes) {
    if (!bytes || bytes > SIZE_MAX - (NXVM_PAGE_SIZE - 1)) {
        last_result = 0;
        return fail(EINVAL);
    }
    return nxvm_release(base, (bytes + NXVM_PAGE_SIZE - 1) & ~(size_t)(NXVM_PAGE_SIZE - 1));
}

void nxvm_get_memory_stats(struct nxvm_memory_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    mutexLock(&memory_lock);
    for (struct reservation *r = reservations; r; r = r->next) {
        ++stats->reservations;
        stats->backing_bytes += r->size;
        stats->reserved_bytes += r->live_pages * NXVM_PAGE_SIZE;
        for (size_t i = 0; i < r->size / NXVM_PAGE_SIZE; ++i)
            if (r->pages[i] & PAGE_COMMITTED) stats->committed_bytes += NXVM_PAGE_SIZE;
    }
    mutexUnlock(&memory_lock);
}

uint32_t nxvm_last_result(void) { return last_result; }
