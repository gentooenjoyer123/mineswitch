/* Read-only diagnostics for the pinned newlib allocator; never allocate here. */
#ifndef MINESWITCH_NEWLIB_FREE_SNAPSHOT_H
#define MINESWITCH_NEWLIB_FREE_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>
struct _reent;
enum { NXALLOC_BUCKETS = 9 };
/* Chunk-size buckets: <4K, <64K, <256K, <1M, <2M, <4M, <16M, <64M, rest.
 * Sizes include allocator headers; largest_request accounts for their cost
 * and the minimum remainder required for the special top chunk. */
struct nxalloc_snapshot {
    size_t requested, padded, arena_bytes, free_bytes, chunks;
    size_t largest_chunk, largest_request, top_bytes, fitting_chunks;
    size_t count[NXALLOC_BUCKETS], bytes[NXALLOC_BUCKETS];
    unsigned invalid, truncated;
};
void mineswitch_newlib_snapshot(struct _reent*, struct nxalloc_snapshot*);
void mineswitch_newlib_failure_reset(void);
int mineswitch_newlib_failure_take(struct nxalloc_snapshot*);
/* Called only by _malloc_r with its actual recursive allocator lock held. */
void mineswitch_newlib_allocation_failed(struct _reent*, size_t, size_t);
#endif
