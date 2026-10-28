/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#ifndef MINESWITCH_HORIZON_FILE_SHARE_H
#define MINESWITCH_HORIZON_FILE_SHARE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Internal SD descriptor implementation. The returned descriptor is allocated
 * by the real newlib handle manager and dispatches through a registered device. */
int nxio_shared_open(const char *path, int flags, int mode, int *handled);
int nxio_shared_descriptor(int fd);
char *nxio_shared_path(int fd);
int nxio_shared_stat_path(const char *canonical, struct stat *info, int *found);
/* Hold a backing transaction across positioned/vector operations. The backing
 * mutex is recursive because newlib callbacks take it for plain libc callers. */
void *nxio_shared_begin(int fd);
void nxio_shared_end(void *backing);
/* Local ownership bookkeeping supplements, never replaces, native exclusion.
 * The caller validates real native writer exclusion before acquiring a claim. */
int nxio_shared_claim(int fd, int acquire);
/* One page slab per read-only JAR backing, lazily allocated and globally bounded.
 * Event counters are cumulative; cache_bytes/cache_peak_bytes are allocation
 * gauges. Relaxed snapshots need not describe one instant and take no
 * registry/backing lock or filesystem IO. */
#define NXIO_JAR_CACHE_PAGE_BYTES (4u * 1024u)
#define NXIO_JAR_CACHE_PAGES 8u
#define NXIO_JAR_CACHE_BACKING_BYTES (NXIO_JAR_CACHE_PAGE_BYTES * NXIO_JAR_CACHE_PAGES)
/* Retained allocation-size name for callers of the initial cache interface. */
#define NXIO_JAR_READAHEAD_BYTES NXIO_JAR_CACHE_BACKING_BYTES
#define NXIO_JAR_CACHE_BUDGET_BYTES (8u * 1024u * 1024u)
struct nxio_shared_io_stats {
    uint64_t logical_reads, native_reads, native_stats;
    uint64_t cache_hits, size_cache_hits;
    uint64_t logical_bytes, native_bytes;
    uint64_t cache_bytes, cache_peak_bytes;
    uint64_t native_read_ns,native_stat_ns;
    uint64_t whole_files, whole_bytes, whole_hits, whole_trims;
};
void nxio_shared_io_snapshot(struct nxio_shared_io_stats *out);
/* Whole-JAR cache: a read-only .jar is loaded with large sequential reads on
 * first use, because SD latency per small read dominates class loading. It is
 * released entirely under native memory pressure (and then stays off). */
#define NXIO_JAR_WHOLE_BUDGET_BYTES (256ull * 1024u * 1024u)
#define NXIO_JAR_WHOLE_FILE_MAX_BYTES (64ull * 1024u * 1024u)
#define NXIO_JAR_WHOLE_CHUNK_BYTES (4u * 1024u * 1024u)
#define NXIO_JAR_WHOLE_MIN_FREE_BYTES (448ull * 1024u * 1024u)
size_t nxio_jar_cache_trim(void);
/* Enables (default) or disables new whole-JAR copies; tests use it to isolate page-cache behavior. */
void nxio_jar_whole_configure(int enabled);
#ifdef __cplusplus
}
#endif
#endif
