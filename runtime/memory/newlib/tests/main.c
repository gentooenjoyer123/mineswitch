/* Exercise genuine Newlib allocation algorithms over real, bounded host
 * mappings. This validates source behavior, not Horizon syscall execution. */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <reent.h>
#include "../free_snapshot.h"

struct mallinfo {
    size_t arena, ordblks, smblks, hblks, hblkhd;
    size_t usmblks, fsmblks, uordblks, fordblks, keepcost;
};
void *_malloc_r(struct _reent *, size_t);
void *_realloc_r(struct _reent *, void *, size_t);
void *_memalign_r(struct _reent *, size_t, size_t);
void *_calloc_r(struct _reent *, size_t, size_t);
void _free_r(struct _reent *, void *);
struct mallinfo _mallinfo_r(struct _reent *);
int _mallopt_r(struct _reent *, int, int);

static const size_t page = 4096;
static const size_t mib = 1024u * 1024u;
static const size_t gib = (size_t)1024u * 1024u * 1024u;
static unsigned checks;
static struct _reent reent;
static unsigned char *mapping, *arena;
static size_t capacity, used, mapped_length, sbrk_calls, failed_growths;
static size_t lock_calls, unlock_calls;
static unsigned lock_depth;
static pthread_mutex_t malloc_mutex;

static void check(int ok, const char *name) {
    if (!ok) {
        fprintf(stderr, "FAIL %s reent_errno=%d errno=%d depth=%u sbrk_calls=%zu\n",
                name, reent._errno, errno, lock_depth, sbrk_calls);
        exit(1);
    }
    ++checks;
}
static void balanced(void) {
    check(lock_depth == 0 && lock_calls == unlock_calls, "malloc lock balance");
}
void __malloc_lock(struct _reent *r) {
    (void)r;
    if (pthread_mutex_lock(&malloc_mutex) != 0) abort();
    ++lock_depth; ++lock_calls;
}
void __malloc_unlock(struct _reent *r) {
    (void)r;
    if (!lock_depth) abort();
    --lock_depth; ++unlock_calls;
    if (pthread_mutex_unlock(&malloc_mutex) != 0) abort();
}
void *_sbrk_r(struct _reent *r, ptrdiff_t change) {
    ++sbrk_calls;
    unsigned char *old = arena + used;
    size_t next;
    if (change >= 0) {
        if ((size_t)change > capacity - used) {
            ++failed_growths; r->_errno = ENOMEM; return (void *)-1;
        }
        next = used + (size_t)change;
    } else {
        /* A widened positive request must never wrap into a giant shrink. */
        size_t amount = (size_t)(-(change + 1)) + 1;
        check(amount <= used, "sbrk never receives wrapped negative growth");
        next = used - amount;
    }
    size_t old_pages = (used + page - 1) & ~(page - 1);
    size_t new_pages = (next + page - 1) & ~(page - 1);
    if (new_pages > old_pages && mprotect(arena + old_pages, new_pages - old_pages, PROT_READ | PROT_WRITE) != 0) {
        r->_errno = errno; return (void *)-1;
    }
    if (new_pages < old_pages) {
        check(mprotect(arena + new_pages, old_pages - new_pages, PROT_NONE) == 0, "real unused arena pages lose access");
        check(madvise(arena + new_pages, old_pages - new_pages, MADV_DONTNEED) == 0, "real released arena pages discard contents");
    }
    used = next;
    return old;
}
static void initialize(void) {
    check(sizeof(size_t) == 8 && sizeof(ptrdiff_t) == 8 && sizeof(unsigned long) == 8,
          "host represents the target allocator's LP64 sizes");
    check(sizeof(struct mallinfo) == 10 * sizeof(size_t), "mallinfo has ten target-width fields");
    check(sysconf(_SC_PAGESIZE) == (long)page, "host uses 4 KiB mapping pages");
    pthread_mutexattr_t attr;
    check(pthread_mutexattr_init(&attr) == 0 && pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
          pthread_mutex_init(&malloc_mutex, &attr) == 0 && pthread_mutexattr_destroy(&attr) == 0,
          "allocator uses an actual recursive lock");
    capacity = 3 * gib + 16 * mib;
    mapped_length = capacity + 2 * page;
    mapping = mmap(NULL, mapped_length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    check(mapping != MAP_FAILED, "reserve bounded host arena with inaccessible guard pages");
    arena = mapping + page;
}
static int repeated(const unsigned char *p, size_t count, unsigned char value) {
    for (size_t i = 0; i < count; ++i) if (p[i] != value) return 0;
    return 1;
}
static void large_allocations(void) {
    check(_mallopt_r(&reent, -1, -1) == 1, "retain released top space for deterministic in-place growth");
    balanced();
    const size_t alignment = 2 * mib, original = 2 * gib + 8 * mib;
    unsigned char *p = _memalign_r(&reent, alignment, original);
    check(p != NULL && (uintptr_t)p % alignment == 0, "large aligned allocation above 2 GiB");
    balanced();
    memset(p, 0x17, page); memset(p + original - page, 0xe3, page);
    memset(p + 2 * gib - page, 0x6a, page);
    check(repeated(p, page, 0x17) && repeated(p + original - page, page, 0xe3), "large allocation first and last pages hold real data");
    struct mallinfo info = _mallinfo_r(&reent);
    check(info.arena >= original && info.uordblks >= original && info.arena <= capacity,
          "allocator metadata reports real sizes above signed 32-bit range");
    unsigned char *smaller = _realloc_r(&reent, p, 2 * gib);
    check(smaller == p && repeated(smaller, page, 0x17) && repeated(smaller + 2 * gib - page, page, 0x6a),
          "shrink realloc to exactly 2 GiB preserves actual prefix and last retained page");
    balanced();
    const size_t expanded = 2 * gib + 4 * mib;
    unsigned char *larger = _realloc_r(&reent, smaller, expanded);
    check(larger == smaller && repeated(larger, page, 0x17) && repeated(larger + 2 * gib - page, page, 0x6a),
          "grow realloc above 2 GiB reuses genuine adjacent free memory");
    memset(larger + expanded - page, 0x81, page);
    check(repeated(larger + expanded - page, page, 0x81), "grown large allocation exposes real writable last page");
    size_t failures = failed_growths;
    check(_realloc_r(&reent, larger, capacity + page) == NULL && reent._errno == ENOMEM && failed_growths > failures,
          "oversized growth encounters genuine bounded arena failure");
    balanced();
    check(repeated(larger, page, 0x17) && repeated(larger + expanded - page, page, 0x81),
          "failed large realloc preserves ownership and first/last data");
    _free_r(&reent, larger); balanced();
    p = _memalign_r(&reent, page, 2 * gib);
    check(p != NULL && (uintptr_t)p % page == 0, "exact 2 GiB page-aligned allocation reuses released large chunk");
    p[0] = 0x43; p[2 * gib - 1] = 0x92;
    check(p[0] == 0x43 && p[2 * gib - 1] == 0x92, "exact 2 GiB allocation endpoints are distinct real bytes");
    _free_r(&reent, p); balanced();
}
static void invalid_requests(void) {
    const size_t sizes[] = {SIZE_MAX, SIZE_MAX - 1, SIZE_MAX - 31, (size_t)PTRDIFF_MAX,
                            (size_t)PTRDIFF_MAX - 16, (size_t)PTRDIFF_MAX - 32,
                            (size_t)PTRDIFF_MAX - 4095};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t calls = sbrk_calls;
        check(_malloc_r(&reent, sizes[i]) == NULL && reent._errno == ENOMEM, "invalid malloc size rejected honestly");
        check(sbrk_calls == calls, "invalid malloc cannot reach sbrk"); balanced();
    }
    unsigned char *p = _malloc_r(&reent, 128);
    check(p != NULL, "small live allocation before invalid realloc"); memset(p, 0x59, 128);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t calls = sbrk_calls;
        check(_realloc_r(&reent, p, sizes[i]) == NULL && reent._errno == ENOMEM, "invalid realloc size rejected honestly");
        balanced();
        check(sbrk_calls == calls && repeated(p, 128, 0x59), "invalid realloc preserves allocation and never reaches sbrk");
    }
    size_t calls = sbrk_calls;
    check(_calloc_r(&reent, SIZE_MAX, 2) == NULL && reent._errno == ENOMEM && sbrk_calls == calls,
          "calloc multiplication overflow stays rejected without backend work");
    check(_memalign_r(&reent, page, SIZE_MAX) == NULL && reent._errno == ENOMEM && sbrk_calls == calls,
          "memalign size overflow stays rejected without backend work");
    balanced();
    _free_r(&reent, p); balanced();
    p = _malloc_r(&reent, 128);
    check(p != NULL, "allocator remains usable after invalid requests"); _free_r(&reent, p); balanced();
}
static void invalid_top_pad(void) {
    check(_mallopt_r(&reent, -2, -1) == 1, "select overflowing top-pad request through genuine allocator API");
    size_t calls = sbrk_calls;
    check(_malloc_r(&reent, 64) == NULL && reent._errno == ENOMEM && sbrk_calls == calls,
          "top-pad overhead overflow rejected before any signed sbrk call");
    balanced();
    check(_mallopt_r(&reent, -2, 0) == 1, "restore ordinary allocator top pad");
    void *p = _malloc_r(&reent, 64);
    check(p != NULL, "valid allocation succeeds after top-pad rejection"); _free_r(&reent, p); balanced();
}
static void small_allocations(void) {
    enum { count = 96 };
    unsigned char *p[count]; size_t sizes[count];
    for (unsigned i = 0; i < count; ++i) {
        sizes[i] = 1 + (i * 97u) % 4093u;
        p[i] = _malloc_r(&reent, sizes[i]);
        check(p[i] != NULL && (uintptr_t)p[i] % 16 == 0, "ordinary malloc retains target alignment");
        memset(p[i], (int)i, sizes[i]); balanced();
    }
    for (unsigned i = 0; i < count; i += 2) {
        check(repeated(p[i], sizes[i], (unsigned char)i), "ordinary allocated bytes survive unrelated allocations");
        _free_r(&reent, p[i]); p[i] = NULL; balanced();
    }
    for (unsigned i = 1; i < count; i += 2) {
        size_t n = sizes[i] + 5000;
        unsigned char *q = _realloc_r(&reent, p[i], n);
        check(q != NULL && repeated(q, sizes[i], (unsigned char)i), "ordinary realloc preserves copied data through fragmentation");
        memset(q + sizes[i], 0xc1, n - sizes[i]);
        check(repeated(q + sizes[i], n - sizes[i], 0xc1), "ordinary grown allocation has real writable tail");
        _free_r(&reent, q); balanced();
    }
    unsigned char *zero = _calloc_r(&reent, 257, 17);
    check(zero != NULL && repeated(zero, 257 * 17, 0), "calloc clears genuine reused arena bytes");
    _free_r(&reent, zero); balanced();
    void *empty = _malloc_r(&reent, 0);
    check(empty != NULL, "Newlib zero-size malloc behavior stays intact"); _free_r(&reent, empty); balanced();
    for (size_t alignment = 16; alignment <= 2 * mib; alignment *= 2) {
        unsigned char *q = _memalign_r(&reent, alignment, 7919);
        check(q != NULL && (uintptr_t)q % alignment == 0, "ordinary memalign covers all requested power-of-two alignments");
        memset(q, 0x3d, 7919); check(repeated(q, 7919, 0x3d), "aligned allocation stores actual complete small contents");
        _free_r(&reent, q); balanced();
    }
}
static void aligned_fallback(int prefix) {
    capacity = 32 * mib;
    const size_t alignment = 2 * mib, original_size = 3 * mib;
    unsigned char *original = _memalign_r(&reent, alignment, original_size);
    check(original != NULL && (uintptr_t)original % alignment == 0, "own aligned fallback fixture");
    const uintptr_t address = (uintptr_t)original;
    unsigned char *fillers[512];
    unsigned filled = 0;
    while (filled < 512) {
        unsigned char *p = _malloc_r(&reent, 65536);
        if (!p) break;
        p[0] = 0x37; p[65535] = 0x82;
        fillers[filled++] = p;
    }
    check(filled < 512, "aligned fixture reaches real heap boundary");
    _free_r(&reent, original);
    void *head = prefix ? _malloc_r(&reent, mib / 2) : NULL;
    if (prefix) check(head && (uintptr_t)head <= address && address-(uintptr_t)head < 65536,
                      "prefix allocation establishes misaligned free interval");
    size_t requested = prefix ? mib : 2 * mib;
    size_t failures = failed_growths;
    unsigned char *p = _memalign_r(&reent, alignment, requested);
    check(p != NULL, "exact aligned interval survives worst-case-padding failure");
    check(failed_growths > failures, "aligned result follows a genuine failed padded allocation");
    check((uintptr_t)p == address + (prefix ? alignment : 0), "aligned fallback returns exact owned interval");
    memset(p, 0x64, requested);
    check(repeated(p, requested, 0x64), "entire aligned fallback allocation is writable");
    for (unsigned i = 0; i < filled; ++i)
        check(fillers[i][0] == 0x37 && fillers[i][65535] == 0x82, "aligned split preserves every neighboring allocation");
    _free_r(&reent, p);
    if (head) _free_r(&reent, head);
    for (unsigned i = 0; i < filled; ++i) _free_r(&reent, fillers[i]);
    p = _malloc_r(&reent, 24 * mib);
    check(p != NULL, "aligned fallback releases and coalesces into large reusable chunk");
    _free_r(&reent, p);
    failures = sbrk_calls;
    check(_memalign_r(&reent, SIZE_MAX, 4096) == NULL && reent._errno == ENOMEM && sbrk_calls == failures,
          "impossible alignment is rejected without wrapped arithmetic or heap access");
    balanced();
}
static void fragmented_metaspace(void) {
    // Genuine free chunks separated by live allocations: total free memory
    // must not be mistaken for one contiguous, aligned reservation.
    capacity = 384 * mib;
    void *blocks[8], *fillers[128];
    unsigned filled = 0;
    for (unsigned i = 0; i < 8; ++i) {
        blocks[i] = _malloc_r(&reent, 40 * mib);
        check(blocks[i] != NULL, "fragmentation setup owns real large chunks");
    }
    while (filled < 128) {
        void *p = _malloc_r(&reent, mib);
        if (!p) break;
        fillers[filled++] = p;
    }
    check(filled < 128, "fragmentation fixture reaches bounded heap end");
    for (unsigned i = 0; i < 8; i += 2) {
        _free_r(&reent, blocks[i]); blocks[i] = NULL;
    }
    check(_mallinfo_r(&reent).fordblks >= 160 * mib, "at least 160 MiB genuinely free across holes");
    check(_memalign_r(&reent, 16 * mib, 64 * mib) == NULL,
          "64 MiB aligned metaspace cannot fit despite enough total free bytes");
    unsigned char *node = _memalign_r(&reent, 16 * mib, 16 * mib);
    check(node != NULL && (uintptr_t)node % (16 * mib) == 0,
          "one root metaspace node fits fragmented heap with exact alignment");
    memset(node, 0x69, page); memset(node + 16 * mib - page, 0x96, page);
    check(repeated(node, page, 0x69) && repeated(node + 16 * mib - page, page, 0x96),
          "fragmented aligned node has real distinct writable ends");
    _free_r(&reent, node);
    for (unsigned i = 1; i < 8; i += 2) _free_r(&reent, blocks[i]);
    for (unsigned i = 0; i < filled; ++i) _free_r(&reent, fillers[i]);
    balanced();
}
static void free_snapshot(void) {
    struct nxalloc_snapshot snap;
    mineswitch_newlib_snapshot(&reent,&snap);
    check(snap.arena_bytes==0 && snap.free_bytes==0 && snap.chunks==0 && !snap.invalid,
          "empty allocator snapshot touches no nonexistent arena");
    check(_mallopt_r(&reent,-1,-1)==1,"retain coalesced fixture instead of returning its top to sbrk");
    capacity=384*mib;
    unsigned char *blocks[512];
    unsigned count=0;
    while (count<512) {
        unsigned char *p=_malloc_r(&reent,mib);
        if (!p) break;
        p[0]=0x37;p[mib-1]=0x92;blocks[count++]=p;
    }
    check(count>320 && count<512,"fragmentation fixture reaches actual arena boundary");
    for (unsigned i=0;i<count;i+=2) { _free_r(&reent,blocks[i]);blocks[i]=NULL; }
    mineswitch_newlib_failure_reset();
    check(_realloc_r(&reent,NULL,5*mib/2)==NULL && reent._errno==ENOMEM,
          "native2.5MiB request really fails despite hundreds of freeMiB");
    check(mineswitch_newlib_failure_take(&snap),"failure captured under actual allocator lock");
    check(snap.requested==5*mib/2 && snap.padded>snap.requested && !snap.invalid && !snap.truncated,
          "locked failure snapshot has exact request and valid complete list");
    check(snap.free_bytes>160*mib && snap.largest_request<5*mib/2 && snap.fitting_chunks==0,
          "snapshot distinguishes aggregate capacity from contiguous-fit failure");
    struct mallinfo info=_mallinfo_r(&reent);
    check(snap.free_bytes==info.fordblks && snap.arena_bytes==info.arena && snap.top_bytes==info.keepcost,
          "independent snapshot agrees with genuine allocator totals");
    size_t bytes=0,entries=0;
    for (unsigned i=0;i<NXALLOC_BUCKETS;i++) { bytes+=snap.bytes[i];entries+=snap.count[i]; }
    check(bytes==snap.free_bytes && entries==snap.chunks,"histogram includes every free chunk exactly once");
    check(!mineswitch_newlib_failure_take(&snap),"failure sample consumed only once");
    unsigned char *p=_malloc_r(&reent,snap.largest_request);
    check(p!=NULL,"reported largest request is a genuinely usable allocation");
    p[0]=0x57;p[snap.largest_request-1]=0x75;
    check(p[0]==0x57 && p[snap.largest_request-1]==0x75,"largest allocation has writable endpoints");
    _free_r(&reent,p);
    for (unsigned i=1;i<count;i+=2) {
        check(blocks[i][0]==0x37 && blocks[i][mib-1]==0x92,"snapshot preserves live allocation data");
        _free_r(&reent,blocks[i]);
    }
    mineswitch_newlib_snapshot(&reent,&snap);
    check(!snap.invalid && !snap.truncated && snap.chunks==1 && snap.largest_request>300*mib,
          "released separators coalesce into one truly large free block");
    p=_malloc_r(&reent,5*mib/2);
    check(p!=NULL,"same2.5MiB request succeeds after real coalescing");
    _free_r(&reent,p);balanced();
}
static void* snapshot_worker(void* arg) {
    struct _reent local={0};
    size_t request=capacity+((uintptr_t)arg+1)*mib;
    for (unsigned i=0;i<100;i++) {
        mineswitch_newlib_failure_reset();
        if (_malloc_r(&local,request)!=NULL || local._errno!=ENOMEM) abort();
        struct nxalloc_snapshot snap;
        if (!mineswitch_newlib_failure_take(&snap) || snap.requested!=request ||
            snap.invalid || snap.truncated || snap.fitting_chunks) abort();
        if (mineswitch_newlib_failure_take(&snap)) abort();
    }
    return NULL;
}
static void snapshot_threads(void) {
    capacity=32*mib;
    void* p=_malloc_r(&reent,mib);
    check(p!=NULL,"thread fixture has a real initialized heap");
    pthread_t threads[4];
    for (uintptr_t i=0;i<4;i++) check(pthread_create(&threads[i],NULL,snapshot_worker,(void*)i)==0,
                                     "start independent failure reporter");
    for (unsigned i=0;i<4;i++) check(pthread_join(threads[i],NULL)==0,"join independent failure reporter");
    struct nxalloc_snapshot snap;
    check(!mineswitch_newlib_failure_take(&snap),"worker failures never leak into another thread");
    _free_r(&reent,p);balanced();
}
int main(int argc, char **argv) {
    check(argc == 2, "one allocator scenario selected"); initialize();
    if (!strcmp(argv[1], "large")) large_allocations();
    else if (!strcmp(argv[1], "invalid")) invalid_requests();
    else if (!strcmp(argv[1], "top-pad")) invalid_top_pad();
    else if (!strcmp(argv[1], "small")) small_allocations();
    else if (!strcmp(argv[1], "metaspace")) fragmented_metaspace();
    else if (!strcmp(argv[1], "aligned-tail")) aligned_fallback(0);
    else if (!strcmp(argv[1], "aligned-prefix")) aligned_fallback(1);
    else if (!strcmp(argv[1], "free-snapshot")) free_snapshot();
    else if (!strcmp(argv[1], "snapshot-threads")) snapshot_threads();
    else check(0, "known allocator scenario");
    balanced();
    check(munmap(mapping, mapped_length) == 0 && pthread_mutex_destroy(&malloc_mutex) == 0, "release genuine bounded host arena and lock");
    printf("NEWLIB_HOST_CASE_PASS case=%s checks=%u sbrk_calls=%zu\n", argv[1], checks, sbrk_calls);
    return 0;
}
