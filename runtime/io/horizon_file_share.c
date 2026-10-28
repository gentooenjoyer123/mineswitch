/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "horizon_file_share.h"
#include "horizon_file_io.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <unistd.h>
#include <time.h>
#include <switch/kernel/mutex.h>
#include <switch/runtime/devices/fs_dev.h>

struct logical_file;
struct cache_page {
    off_t offset;
    size_t length;
    uint64_t last_used;
};
struct shared_file {
    struct shared_file *next;
    const devoptab_t *device;
    void *native_file;
    char *path;
    RMutex lock;
    unsigned references;
    int writable;
    int opened;
    int jar;
    int size_valid;
    off_t stable_size;
    char *cache;
    char *whole;     /* Guarded by whole_lock; owned while listed in whole_entries. */
    int whole_state; /* 0 untried, 1 loaded, 2 not cacheable or released */
    struct cache_page pages[NXIO_JAR_CACHE_PAGES];
    uint64_t cache_clock;
    struct logical_file *claim_owner;
};
struct logical_file {
    struct shared_file *file;
    off_t position;
    int flags;
};
static Mutex registry_lock;
static struct shared_file *registry;
static int shared_device = -1;
static struct nxio_shared_io_stats io_stats;
static uint64_t cache_reserved_bytes;
/* Lock order: backing lock -> whole_lock. Trimming takes only whole_lock. */
static Mutex whole_lock;
static struct shared_file *whole_entries[1024];
static unsigned whole_count;
static uint64_t whole_reserved;
static int whole_disabled;
static int whole_registered;
static uint64_t monotonic_ns(void) {
    struct timespec t;
    int saved_errno=errno;
    int rc=clock_gettime(CLOCK_MONOTONIC,&t);
    errno=saved_errno;
    return rc==0?(uint64_t)t.tv_sec*1000000000+t.tv_nsec:0;
}

#define COUNT(field, value) __atomic_fetch_add(&io_stats.field, (uint64_t)(value), __ATOMIC_RELAXED)
void nxio_shared_io_snapshot(struct nxio_shared_io_stats *out) {
    if (!out) return;
#define SNAPSHOT(field) out->field = __atomic_load_n(&io_stats.field, __ATOMIC_RELAXED)
    SNAPSHOT(logical_reads); SNAPSHOT(native_reads); SNAPSHOT(native_stats);
    SNAPSHOT(cache_hits); SNAPSHOT(size_cache_hits);
    SNAPSHOT(logical_bytes); SNAPSHOT(native_bytes);
    SNAPSHOT(cache_bytes); SNAPSHOT(cache_peak_bytes);
    SNAPSHOT(native_read_ns); SNAPSHOT(native_stat_ns);
    SNAPSHOT(whole_files); SNAPSHOT(whole_bytes); SNAPSHOT(whole_hits); SNAPSHOT(whole_trims);
#undef SNAPSHOT
}
static int cacheable(const struct shared_file *file) {
    return file->jar && file->opened && !file->writable;
}
static int jar_path(const char *path) {
    size_t length = strlen(path);
    return length >= 4 && path[length - 4] == '.' &&
        (path[length - 3] | 32) == 'j' && (path[length - 2] | 32) == 'a' && (path[length - 1] | 32) == 'r';
}
/* Always called under the backing lock (or before publishing a new backing).
 * The SDK read handle excludes foreign writers only until its next close. */
static void drop_whole(struct shared_file *file) {
    mutexLock(&whole_lock);
    for (unsigned i = 0; i < whole_count; ++i) {
        if (whole_entries[i] != file) continue;
        whole_entries[i] = whole_entries[--whole_count];
        whole_reserved -= (uint64_t)file->stable_size;
        __atomic_fetch_sub(&io_stats.whole_bytes, (uint64_t)file->stable_size, __ATOMIC_RELAXED);
        free(file->whole);
        file->whole = NULL;
        break;
    }
    mutexUnlock(&whole_lock);
    file->whole_state = 0;
}
static void invalidate_cache(struct shared_file *file) {
    drop_whole(file);
    file->size_valid = 0;
    if (file->cache) {
        memset(file->pages, 0, sizeof(file->pages));
        file->cache_clock = 0;
        free(file->cache);
        file->cache = NULL;
        __atomic_fetch_sub(&io_stats.cache_bytes, NXIO_JAR_CACHE_BACKING_BYTES, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&cache_reserved_bytes, NXIO_JAR_CACHE_BACKING_BYTES, __ATOMIC_RELAXED);
    }
}
static int allocate_cache(struct shared_file *file) {
    if (file->cache) return 1;
    uint64_t reserved = __atomic_load_n(&cache_reserved_bytes, __ATOMIC_RELAXED);
    do {
        if (reserved > NXIO_JAR_CACHE_BUDGET_BYTES - NXIO_JAR_CACHE_BACKING_BYTES) return 0;
    } while (!__atomic_compare_exchange_n(&cache_reserved_bytes, &reserved, reserved + NXIO_JAR_CACHE_BACKING_BYTES,
                                          1, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    int saved_errno = errno;
    file->cache = malloc(NXIO_JAR_CACHE_BACKING_BYTES);
    errno = saved_errno;
    if (!file->cache) {
        __atomic_fetch_sub(&cache_reserved_bytes, NXIO_JAR_CACHE_BACKING_BYTES, __ATOMIC_RELAXED);
        return 0;
    }
    uint64_t current = __atomic_add_fetch(&io_stats.cache_bytes, NXIO_JAR_CACHE_BACKING_BYTES, __ATOMIC_RELAXED);
    uint64_t peak = __atomic_load_n(&io_stats.cache_peak_bytes, __ATOMIC_RELAXED);
    while (peak < current && !__atomic_compare_exchange_n(&io_stats.cache_peak_bytes, &peak, current,
                                                         1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return 1;
}

static int fail(struct _reent *r, int error) {
    r->_errno = error;
    errno = error;
    return -1;
}
static int native_error(struct _reent *r) {
    int error = r->_errno;
    if (error == EIO && fsdevGetLastResult() == 0xe02) error = EACCES;
    return fail(r, error);
}
static int native_open(struct _reent *r, struct shared_file *file, void *state, int flags, int mode) {
    void *previous = r->deviceData;
    r->deviceData = file->device->deviceData;
    int result = file->device->open_r(r, state, file->path, flags, mode);
    r->deviceData = previous;
    return result < 0 ? native_error(r) : result;
}
static int native_close(struct _reent *r, struct shared_file *file) {
    invalidate_cache(file);
    void *previous = r->deviceData;
    r->deviceData = file->device->deviceData;
    int result = file->device->close_r(r, file->native_file);
    r->deviceData = previous;
    file->opened = 0;
    return result;
}
static int ensure_open(struct _reent *r, struct shared_file *file) {
    if (file->opened) return 0;
    invalidate_cache(file);
    /* A foreign writer can win the unavoidable gap in a failed read-to-write
     * upgrade. Existing channels keep their logical positions; retry the real
     * reader open instead of returning fabricated data or success. */
    if (native_open(r, file, file->native_file, O_RDONLY, 0) < 0) return -1;
    file->opened = 1;
    file->writable = 0;
    return 0;
}
static int native_stat(struct _reent *r, struct shared_file *file, struct stat *info) {
    if (ensure_open(r, file) < 0) return -1;
    void *previous = r->deviceData;
    r->deviceData = file->device->deviceData;
    COUNT(native_stats, 1);
    uint64_t started=monotonic_ns();
    int result = file->device->fstat_r(r, file->native_file, info);
    uint64_t ended=monotonic_ns();if(ended>=started)COUNT(native_stat_ns,ended-started);
    r->deviceData = previous;
    if (result == 0 && cacheable(file) && info->st_size >= 0) {
        file->stable_size = info->st_size;
        file->size_valid = 1;
    }
    return result;
}
static int read_size(struct _reent *r, struct shared_file *file, off_t *size) {
    if (cacheable(file) && file->size_valid) {
        COUNT(size_cache_hits, 1);
        *size = file->stable_size;
        return 0;
    }
    struct stat info;
    if (native_stat(r, file, &info) < 0) return -1;
    if (info.st_size < 0) return fail(r, EIO);
    *size = info.st_size;
    return 0;
}
static ssize_t native_read_at(struct _reent *r, struct shared_file *file, off_t offset, char *buffer, size_t size) {
    void *previous = r->deviceData;
    r->deviceData = file->device->deviceData;
    ssize_t result = -1;
    uint64_t started=monotonic_ns();
    if (file->device->seek_r(r, file->native_file, offset, SEEK_SET) >= 0) {
        COUNT(native_reads, 1);
        result = file->device->read_r(r, file->native_file, buffer, size);
        if (result > 0) COUNT(native_bytes, result);
    }
    uint64_t ended=monotonic_ns();if(ended>=started)COUNT(native_read_ns,ended-started);
    r->deviceData = previous;
    return result;
}
static ssize_t cached_read_at(struct _reent *r, struct shared_file *file, off_t offset,
                              char *buffer, size_t size, off_t length) {
    size_t copied = 0;
    int all_cached = 1;
    while (copied < size) {
        off_t position = offset + (off_t)copied;
        size_t within = (size_t)((uint64_t)position % NXIO_JAR_CACHE_PAGE_BYTES);
        off_t page_offset = position - (off_t)within;
        size_t wanted = NXIO_JAR_CACHE_PAGE_BYTES - within;
        if (wanted > size - copied) wanted = size - copied;
        unsigned slot = NXIO_JAR_CACHE_PAGES;
        for (unsigned i = 0; i < NXIO_JAR_CACHE_PAGES; ++i) {
            if (file->pages[i].length && file->pages[i].offset == page_offset) { slot = i; break; }
        }
        if (slot == NXIO_JAR_CACHE_PAGES) {
            slot = 0;
            for (unsigned i = 0; i < NXIO_JAR_CACHE_PAGES; ++i) {
                if (!file->pages[i].length) { slot = i; break; }
                if (file->pages[i].last_used < file->pages[slot].last_used) slot = i;
            }
            struct cache_page *page = &file->pages[slot];
            page->length = 0;
            uint64_t available = (uint64_t)(length - page_offset);
            size_t fill = available < NXIO_JAR_CACHE_PAGE_BYTES ? (size_t)available : NXIO_JAR_CACHE_PAGE_BYTES;
            all_cached = 0;
            ssize_t result = native_read_at(r, file, page_offset, file->cache + slot * NXIO_JAR_CACHE_PAGE_BYTES, fill);
            if (result < 0) return copied ? (ssize_t)copied : -1;
            page->offset = page_offset;
            page->length = (size_t)result;
            // A zero returned at the actual requested offset is a genuine
            // native result. It is never published as a cached EOF.
            if (!result && !within) return (ssize_t)copied;
        }
        struct cache_page *page = &file->pages[slot];
        if (page->length <= within) {
            /* Read-ahead may short-read before the requested in-page offset.
             * Subtracting that prefix would fabricate EOF. Try the actual
             * requested range once, without refilling or caching the result. */
            page->length = 0;
            ssize_t result = native_read_at(r, file, position, buffer + copied, size - copied);
            if (result < 0) return copied ? (ssize_t)copied : -1;
            return (ssize_t)copied + result;
        }
        page->last_used = ++file->cache_clock;
        size_t count = page->length - within;
        if (count > wanted) count = wanted;
        memcpy(buffer + copied, file->cache + slot * NXIO_JAR_CACHE_PAGE_BYTES + within, count);
        copied += count;
        // Preserve the real short transfer; never loop on an incomplete page.
        if (count < wanted) return (ssize_t)copied;
    }
    if (all_cached) COUNT(cache_hits, 1);
    return (ssize_t)copied;
}
size_t nxio_jar_cache_trim(void) {
    /* Called from allocation-failure recovery on arbitrary threads: never block. */
    if (!mutexTryLock(&whole_lock)) return 0;
    size_t released = 0;
    for (unsigned i = 0; i < whole_count; ++i) {
        struct shared_file *file = whole_entries[i];
        released += (size_t)file->stable_size;
        __atomic_fetch_sub(&io_stats.whole_bytes, (uint64_t)file->stable_size, __ATOMIC_RELAXED);
        free(file->whole);
        file->whole = NULL;
        file->whole_state = 2;
    }
    whole_count = 0;
    whole_reserved = 0;
    whole_disabled = 1;
    COUNT(whole_trims, 1);
    mutexUnlock(&whole_lock);
    return released;
}
void nxio_jar_whole_configure(int enabled) {
    mutexLock(&whole_lock);
    whole_disabled = !enabled;
    mutexUnlock(&whole_lock);
}
static size_t trim_callback(void *unused) { (void)unused; return nxio_jar_cache_trim(); }
extern bool mineswitch_nxvk_register_cache(void *, size_t (*)(void *)) __attribute__((weak));
static uint64_t free_native_bytes(void) {
#ifdef __SWITCH__
    struct mallinfo info = mallinfo();
    return (uint64_t)info.fordblks;
#else
    return UINT64_MAX;
#endif
}
/* Under the backing lock. Returns 1 when the whole file is now resident. */
static int load_whole(struct _reent *r, struct shared_file *file, off_t length) {
    if (file->whole_state == 1) return 1;
    if (file->whole_state == 2 || length <= 0 || (uint64_t)length > NXIO_JAR_WHOLE_FILE_MAX_BYTES) {
        file->whole_state = 2;
        return 0;
    }
    mutexLock(&whole_lock);
    int allowed = !whole_disabled && whole_count < sizeof(whole_entries) / sizeof(whole_entries[0]) &&
                  whole_reserved + (uint64_t)length <= NXIO_JAR_WHOLE_BUDGET_BYTES;
    if (allowed) whole_reserved += (uint64_t)length;
    if (!whole_registered && mineswitch_nxvk_register_cache) whole_registered = mineswitch_nxvk_register_cache(&whole_entries, trim_callback);
    mutexUnlock(&whole_lock);
    if (!allowed) { file->whole_state = 2; return 0; }
    if (free_native_bytes() < (uint64_t)length + NXIO_JAR_WHOLE_MIN_FREE_BYTES) {
        /* Memory is getting tight: give back everything cached so far. */
        mutexLock(&whole_lock); whole_reserved -= (uint64_t)length; mutexUnlock(&whole_lock);
        nxio_jar_cache_trim();
        file->whole_state = 2;
        return 0;
    }
    int saved_errno = errno;
    char *data = malloc((size_t)length);
    errno = saved_errno;
    off_t done = 0;
    while (data && done < length) {
        size_t chunk = (size_t)(length - done) < NXIO_JAR_WHOLE_CHUNK_BYTES ? (size_t)(length - done) : NXIO_JAR_WHOLE_CHUNK_BYTES;
        ssize_t got = native_read_at(r, file, done, data + done, chunk);
        if (got <= 0) break;
        done += got;
    }
    mutexLock(&whole_lock);
    if (!data || done != length || whole_disabled) {
        whole_reserved -= (uint64_t)length;
        mutexUnlock(&whole_lock);
        free(data);
        file->whole_state = 2;
        return 0;
    }
    file->whole = data;
    file->whole_state = 1;
    whole_entries[whole_count++] = file;
    COUNT(whole_files, 1);
    COUNT(whole_bytes, length);
    mutexUnlock(&whole_lock);
    return 1;
}
/* Under the backing lock. Returns -1 if the whole copy is unavailable. */
static ssize_t whole_read_at(struct shared_file *file, off_t offset, char *buffer, size_t size, off_t length) {
    ssize_t result = -1;
    mutexLock(&whole_lock);
    if (file->whole && file->stable_size == length) {
        size_t count = offset >= length ? 0 : (size_t)(length - offset) < size ? (size_t)(length - offset) : size;
        memcpy(buffer, file->whole + offset, count);
        result = (ssize_t)count;
        COUNT(whole_hits, 1);
    }
    mutexUnlock(&whole_lock);
    return result;
}
static int native_sync(struct _reent *r, struct shared_file *file) {
    if (ensure_open(r, file) < 0) return -1;
    void *previous = r->deviceData;
    r->deviceData = file->device->deviceData;
    int result = file->device->fsync_r(r, file->native_file);
    r->deviceData = previous;
    return result;
}
static int native_truncate(struct _reent *r, struct shared_file *file, off_t length) {
    invalidate_cache(file);
    if (ensure_open(r, file) < 0) return -1;
    void *previous = r->deviceData;
    r->deviceData = file->device->deviceData;
    int result = file->device->ftruncate_r(r, file->native_file, length);
    r->deviceData = previous;
    return result;
}

static int shared_close(struct _reent *r, void *state) {
    struct logical_file *logical = state;
    struct shared_file *file = logical->file;
    mutexLock(&registry_lock);
    rmutexLock(&file->lock);
    if (file->claim_owner == logical) file->claim_owner = NULL;
    int result = 0;
    if (--file->references == 0) {
        struct shared_file **link = &registry;
        while (*link != file) link = &(*link)->next;
        *link = file->next;
        if (file->opened) result = native_close(r, file);
        else invalidate_cache(file);
        drop_whole(file);
        int error = r->_errno;
        rmutexUnlock(&file->lock);
        free(file->native_file);
        free(file->path);
        free(file);
        mutexUnlock(&registry_lock);
        if (result < 0) return fail(r, error);
        return 0;
    }
    rmutexUnlock(&file->lock);
    mutexUnlock(&registry_lock);
    return result;
}
static ssize_t shared_read(struct _reent *r, void *state, char *buffer, size_t size) {
    COUNT(logical_reads, 1);
    struct logical_file *logical = state;
    if ((logical->flags & O_ACCMODE) == O_WRONLY) return fail(r, EBADF);
    if (size > INT64_MAX) return fail(r, EINVAL);
    int allow_readahead = size <= NXIO_JAR_CACHE_BACKING_BYTES;
    struct shared_file *file = logical->file;
    rmutexLock(&file->lock);
    ssize_t result = -1;
    off_t length;
    if (read_size(r, file, &length) == 0) {
        if (logical->position >= length || !size) result = 0;
        else {
            /* Horizon rejects an offset beyond EOF with an I/O error. POSIX
             * and Java require EOF even when another channel truncated the
             * file without changing this channel's position. Query the real
             * size under the backing lock. Only a continuously held native
             * read-only JAR handle permits reusing that verified size. */
            uint64_t available = (uint64_t)(length - logical->position);
            if ((uint64_t)size > available) size = (size_t)available;
            if (cacheable(file) && file->size_valid && load_whole(r, file, length) &&
                (result = whole_read_at(file, logical->position, buffer, size, length)) >= 0) {
                /* Served from the resident copy. */
            } else if (cacheable(file) && allow_readahead && allocate_cache(file)) {
                result = cached_read_at(r, file, logical->position, buffer, size, length);
            } else {
                result = native_read_at(r, file, logical->position, buffer, size);
                if (result < 0) memset(file->pages, 0, sizeof(file->pages));
            }
            if (result > 0) {
                logical->position += result;
                COUNT(logical_bytes, result);
            }
        }
    }
    int error = r->_errno;
    rmutexUnlock(&file->lock);
    if (result < 0) fail(r, error);
    return result;
}
static ssize_t shared_write(struct _reent *r, void *state, const char *buffer, size_t size) {
    struct logical_file *logical = state;
    if ((logical->flags & O_ACCMODE) == O_RDONLY) return fail(r, EBADF);
    if (size > INT64_MAX) return fail(r, EINVAL);
    struct shared_file *file = logical->file;
    rmutexLock(&file->lock);
    invalidate_cache(file);
    ssize_t result = -1;
    off_t position = logical->position;
    if (ensure_open(r, file) < 0) goto done;
    if (!file->writable) { fail(r, EBADF); goto done; }
    if (logical->flags & O_APPEND) {
        struct stat info;
        if (native_stat(r, file, &info) < 0) goto done;
        position = info.st_size;
    }
    if ((uint64_t)position > INT64_MAX - size) { fail(r, EFBIG); goto done; }
    void *previous = r->deviceData;
    r->deviceData = file->device->deviceData;
    if (file->device->seek_r(r, file->native_file, position, SEEK_SET) >= 0) {
        result = file->device->write_r(r, file->native_file, buffer, size);
        if (result >= 0) logical->position = position + result;
    }
    r->deviceData = previous;
    if (result >= 0 && (logical->flags & (O_SYNC | O_DSYNC)) && native_sync(r, file) < 0) result = -1;
done: {
    int error = r->_errno;
    rmutexUnlock(&file->lock);
    if (result < 0) fail(r, error);
    return result;
}}
static off_t shared_seek(struct _reent *r, void *state, off_t offset, int origin) {
    struct logical_file *logical = state;
    struct shared_file *file = logical->file;
    rmutexLock(&file->lock);
    off_t base = 0, result = -1;
    if (origin == SEEK_CUR) base = logical->position;
    else if (origin == SEEK_END) {
        if (read_size(r, file, &base) < 0) goto done;
    } else if (origin != SEEK_SET) { fail(r, EINVAL); goto done; }
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && offset < -base)) { fail(r, EINVAL); goto done; }
    result = logical->position = base + offset;
done: {
    int error = r->_errno;
    rmutexUnlock(&file->lock);
    if (result < 0) fail(r, error);
    return result;
}}
static int shared_stat(struct _reent *r, void *state, struct stat *info) {
    struct shared_file *file = ((struct logical_file*)state)->file;
    rmutexLock(&file->lock);
    int result = native_stat(r, file, info), error = r->_errno;
    rmutexUnlock(&file->lock);
    return result < 0 ? fail(r, error) : result;
}
static int shared_sync(struct _reent *r, void *state) {
    struct shared_file *file = ((struct logical_file*)state)->file;
    rmutexLock(&file->lock);
    int result = native_sync(r, file), error = r->_errno;
    rmutexUnlock(&file->lock);
    return result < 0 ? fail(r, error) : result;
}
static int shared_truncate(struct _reent *r, void *state, off_t length) {
    struct logical_file *logical = state;
    if ((logical->flags & O_ACCMODE) == O_RDONLY) return fail(r, EBADF);
    if (length < 0) return fail(r, EINVAL);
    struct shared_file *file = logical->file;
    rmutexLock(&file->lock);
    int result = native_truncate(r, file, length), error = r->_errno;
    rmutexUnlock(&file->lock);
    return result < 0 ? fail(r, error) : result;
}
static const devoptab_t shared_operations = {
    .name = "nxiofile",
    .structSize = sizeof(struct logical_file),
    .close_r = shared_close,
    .write_r = shared_write,
    .read_r = shared_read,
    .seek_r = shared_seek,
    .fstat_r = shared_stat,
    .ftruncate_r = shared_truncate,
    .fsync_r = shared_sync
};

static struct logical_file *get_logical(int fd) {
    __handle *handle = __get_handle(fd);
    if (!handle || handle->device >= STD_MAX || devoptab_list[handle->device] != &shared_operations) return NULL;
    return handle->fileStruct;
}
int nxio_shared_descriptor(int fd) { return get_logical(fd) != NULL; }
char *nxio_shared_path(int fd) {
    struct logical_file *logical = get_logical(fd);
    if (!logical) { errno = ENOTSUP; return NULL; }
    rmutexLock(&logical->file->lock);
    char *path = strdup(logical->file->path);
    rmutexUnlock(&logical->file->lock);
    return path;
}
int nxio_shared_stat_path(const char *canonical, struct stat *info, int *found) {
    *found = 0;
    mutexLock(&registry_lock);
    struct shared_file *file = registry;
    while (file && strcmp(file->path, canonical)) file = file->next;
    int result = -1, error = errno;
    if (file) {
        *found = 1;
        rmutexLock(&file->lock);
        result = native_stat(_REENT, file, info);
        error = _REENT->_errno;
        rmutexUnlock(&file->lock);
    }
    mutexUnlock(&registry_lock);
    errno = error;
    return result;
}
void *nxio_shared_begin(int fd) {
    struct logical_file *logical = get_logical(fd);
    if (!logical) return NULL;
    rmutexLock(&logical->file->lock);
    return logical->file;
}
void nxio_shared_end(void *backing) {
    if (backing) rmutexUnlock(&((struct shared_file*)backing)->lock);
}
int nxio_shared_claim(int fd, int acquire) {
    struct logical_file *logical = get_logical(fd);
    if (!logical) { errno = ENOTSUP; return -1; }
    struct shared_file *file = logical->file;
    rmutexLock(&file->lock);
    int error = 0;
    if (acquire) {
        if ((logical->flags & O_ACCMODE) == O_RDONLY || !file->opened || !file->writable) error = ENOTSUP;
        else if (file->claim_owner) error = EAGAIN;
        else file->claim_owner = logical;
    } else if (file->claim_owner != logical) error = EINVAL;
    else file->claim_owner = NULL;
    rmutexUnlock(&file->lock);
    if (error) { errno = error; return -1; }
    return 0;
}

static int upgrade(struct _reent *r, struct shared_file *file) {
    invalidate_cache(file);
    if (file->opened && file->writable) return 0;
    if (file->claim_owner) return fail(r, EACCES);
    void *replacement = calloc(1, file->device->structSize);
    if (!replacement) return fail(r, ENOMEM);
    if (file->opened && native_close(r, file) < 0) { free(replacement); return -1; }
    if (native_open(r, file, replacement, O_RDWR, 0) < 0) {
        int error = r->_errno;
        free(replacement);
        /* Restoration failure is kept as a real closed backing. A later IO
         * retries the native read open; no logical descriptor is reassigned. */
        ensure_open(r, file);
        return fail(r, error);
    }
    free(file->native_file);
    file->native_file = replacement;
    file->opened = file->writable = 1;
    return 0;
}

/* Canonicalize an existing file or, for CREATE, its real parent and new leaf. */
static char *canonical_for_open(const char *path, int flags) {
    char *canonical = nxio_canonical_sd_path(path);
    if (canonical || errno != ENOENT || !(flags & O_CREAT)) return canonical;
    FsFileSystem *filesystem;
    char translated[FS_MAX_PATH];
    if (fsdevTranslatePath(path, &filesystem, translated) != 0) return NULL;
    char *slash = strrchr(translated, '/');
    if (!slash || !slash[1]) { errno = ENOENT; return NULL; }
    char *leaf = strdup(slash + 1);
    if (!leaf) return NULL;
    if (slash == translated) slash[1] = 0;
    else *slash = 0;
    char parent[FS_MAX_PATH + 6];
    if (snprintf(parent, sizeof(parent), "sdmc:%s", translated) >= (int)sizeof(parent)) {
        free(leaf); errno = ENAMETOOLONG; return NULL;
    }
    char *base = nxio_canonical_sd_path(parent);
    if (!base) { free(leaf); return NULL; }
    size_t size = strlen(base) + strlen(leaf) + 2;
    canonical = malloc(size);
    if (canonical) snprintf(canonical, size, "%s%s%s", base, base[strlen(base)-1] == '/' ? "" : "/", leaf);
    free(base); free(leaf);
    return canonical;
}

int nxio_shared_open(const char *path, int flags, int mode, int *handled) {
    *handled = 0;
    FsFileSystem *filesystem;
    char translated[FS_MAX_PATH];
    if (fsdevTranslatePath(path, &filesystem, translated) != 0 || filesystem != fsdevGetDeviceFileSystem("sdmc")) return -1;
    FsDirEntryType type;
    Result status = fsFsGetEntryType(filesystem, translated, &type);
    if (R_SUCCEEDED(status) && type != FsDirEntryType_File) return -1;
    *handled = 1;
    struct _reent *r = _REENT;
    if ((flags & O_ACCMODE) != O_RDONLY && (flags & O_ACCMODE) != O_WRONLY && (flags & O_ACCMODE) != O_RDWR)
        return fail(r, EINVAL);
    mutexLock(&registry_lock);
    char *canonical = canonical_for_open(path, flags);
    if (!canonical) { int error = errno; mutexUnlock(&registry_lock); return fail(r, error); }
    if (shared_device < 0) {
        shared_device = AddDevice(&shared_operations);
        if (shared_device < 0) { int error = errno; free(canonical); mutexUnlock(&registry_lock); return fail(r, error); }
    }
    int fd = __alloc_handle(shared_device);
    if (fd < 0) { int error = errno; free(canonical); mutexUnlock(&registry_lock); return fail(r, error); }
    struct logical_file *logical = __get_handle(fd)->fileStruct;
    memset(logical, 0, sizeof(*logical));
    struct shared_file *file = registry;
    while (file && strcmp(file->path, canonical) != 0) file = file->next;
    int created = file == NULL;
    if (created) {
        file = calloc(1, sizeof(*file));
        if (!file) { free(canonical); __release_handle(fd); mutexUnlock(&registry_lock); return fail(r, ENOMEM); }
        file->path = canonical;
        file->device = GetDeviceOpTab(canonical);
        rmutexInit(&file->lock);
        file->native_file = file->device ? calloc(1, file->device->structSize) : NULL;
        if (!file->native_file) { fail(r, ENOMEM); goto error; }
        int physical_flags = flags & ~(O_ACCMODE | O_APPEND | O_SYNC | O_DSYNC);
        file->writable = (flags & O_ACCMODE) != O_RDONLY;
        physical_flags |= file->writable ? O_RDWR : O_RDONLY;
        if (native_open(r, file, file->native_file, physical_flags, mode) < 0) goto error;
        file->opened = 1;
        if (status == 0x202) {
            char *actual = nxio_canonical_sd_path(file->path);
            if (!actual) { fail(r, errno); goto error; }
            free(file->path); file->path = actual;
        }
        file->jar = jar_path(file->path);
        file->next = registry; registry = file;
    } else {
        free(canonical);
        rmutexLock(&file->lock);
        if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) { fail(r, EEXIST); goto existing_error; }
        if ((flags & O_ACCMODE) != O_RDONLY && upgrade(r, file) < 0) goto existing_error;
        if ((flags & O_ACCMODE) == O_RDONLY && ensure_open(r, file) < 0) goto existing_error;
        if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY && native_truncate(r, file, 0) < 0) goto existing_error;
        rmutexUnlock(&file->lock);
    }
    ++file->references;
    logical->file = file;
    logical->flags = flags;
    mutexUnlock(&registry_lock);
    return fd;
existing_error:
    rmutexUnlock(&file->lock);
error: {
    int error = r->_errno;
    if (created) {
        if (file->opened) native_close(r, file);
        free(file->native_file); free(file->path); free(file);
    }
    __release_handle(fd);
    mutexUnlock(&registry_lock);
    return fail(r, error);
}}
