/* Use the audited source's exact chunk/bin declarations without defining any
 * DEFINE_* allocator implementation. The existing overlay owns all state. */
#include <stdint.h>
#include "free_snapshot.h"
#ifndef _LIBC
#define _LIBC 1
#endif
#include "vendor/_mallocr.c"

static _Thread_local struct nxalloc_snapshot failure;
static _Thread_local int failure_valid;

static void observe_chunk(struct nxalloc_snapshot* out, mchunkptr p, int is_top)
{
    size_t size = chunksize(p);
    if (!size) return; /* Initial top is an empty bin sentinel. */
    unsigned bucket = size < 4096 ? 0 : size < 65536 ? 1 : size < 262144 ? 2 :
                      size < 1048576 ? 3 : size < 2097152 ? 4 : size < 4194304 ? 5 :
                      size < 16777216 ? 6 : size < 67108864 ? 7 : 8;
    size_t overhead = SIZE_SZ + (is_top ? MINSIZE : 0);
    size_t usable = size > overhead ? size - overhead : 0;
    out->free_bytes += size;
    out->chunks++;
    out->count[bucket]++;
    out->bytes[bucket] += size;
    if (size > out->largest_chunk) out->largest_chunk = size;
    if (usable > out->largest_request) out->largest_request = usable;
    if (out->padded && size >= out->padded &&
        (!is_top || size - out->padded >= MINSIZE)) out->fitting_chunks++;
}

static int owned_chunk(mchunkptr p, uintptr_t begin, size_t arena)
{
    uintptr_t addr = (uintptr_t)p;
    return addr >= begin && addr - begin <= arena &&
           arena - (addr - begin) >= sizeof(*p) &&
           (((uintptr_t)chunk2mem(p) & MALLOC_ALIGN_MASK) == 0);
}

static int valid_size(mchunkptr p, uintptr_t begin, size_t arena)
{
    size_t size = chunksize(p);
    return size >= MINSIZE && !(size & MALLOC_ALIGN_MASK) &&
           size <= arena - ((uintptr_t)p - begin);
}

/* Caller holds the real allocator lock. No allocation, output, or mutation of
 * allocator state. Bound traversal and validate pointers before dereferencing
 * them so a broken free-list link is reported rather than blindly followed. */
static void snapshot_locked(struct nxalloc_snapshot* out, size_t request, size_t padded)
{
    *out = (struct nxalloc_snapshot){.requested=request, .padded=padded,
                                    .arena_bytes=sbrked_mem};
    if (!sbrked_mem) return;
    uintptr_t begin = (uintptr_t)sbrk_base;
    if (sbrk_base == (char*)-1 || begin > UINTPTR_MAX - sbrked_mem ||
        !owned_chunk(top, begin, sbrked_mem) || !valid_size(top, begin, sbrked_mem)) {
        out->invalid = 1; return;
    }
    out->top_bytes = chunksize(top);
    observe_chunk(out, top, 1);
    for (unsigned i=1; i<NAV; i++) {
        mbinptr bin = bin_at(i);
        for (mchunkptr p=last(bin); p!=bin; p=p->bk) {
            if (out->chunks >= 262144) { out->truncated=1; return; }
            if (!owned_chunk(p, begin, sbrked_mem) || !valid_size(p, begin, sbrked_mem) ||
                (p->bk!=bin && !owned_chunk(p->bk,begin,sbrked_mem)) ||
                (p->fd!=bin && !owned_chunk(p->fd,begin,sbrked_mem))) {
                out->invalid=1; return;
            }
            if (p->bk->fd != p || p->fd->bk != p || p == top) {
                out->invalid=1; return;
            }
            observe_chunk(out, p, 0);
        }
    }
    if (out->free_bytes > out->arena_bytes) out->invalid=1;
}

void mineswitch_newlib_snapshot(struct _reent* reent_ptr, struct nxalloc_snapshot* out)
{
    MALLOC_LOCK;
    snapshot_locked(out,0,0);
    MALLOC_UNLOCK;
}

void mineswitch_newlib_allocation_failed(struct _reent* reent_ptr, size_t request, size_t padded)
{
    (void)reent_ptr;
    snapshot_locked(&failure,request,padded);
    failure_valid=1;
}
void mineswitch_newlib_failure_reset(void) { failure_valid=0; }
int mineswitch_newlib_failure_take(struct nxalloc_snapshot* out)
{
    if (!failure_valid) return 0;
    *out=failure;
    failure_valid=0;
    return 1;
}
