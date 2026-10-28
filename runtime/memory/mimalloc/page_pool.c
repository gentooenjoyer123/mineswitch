#include "page_pool.h"
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#ifdef MINESWITCH_MIMALLOC_HOST
#include <pthread.h>
#include <sys/mman.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
static int permissions(void* p,size_t n,bool access) {
    return mprotect(p,n,access?PROT_READ|PROT_WRITE:PROT_NONE)==0?0:errno;
}
#else
#include <switch.h>
static Mutex lock;
#define LOCK() mutexLock(&lock)
#define UNLOCK() mutexUnlock(&lock)
static int permissions(void* p,size_t n,bool access) {
    return R_SUCCEEDED(svcSetMemoryPermission(p,n,access?Perm_Rw:Perm_None))?0:EACCES;
}
#endif

enum { PAGE=4096, MAX_PAGES=(8ULL*1024*1024*1024)/PAGE, WORDS=MAX_PAGES/64 };
static uint64_t allocated[WORDS], accessible[WORDS];
static uintptr_t pool;
static size_t pages, reserved_pages, access_pages, peak_pages, alloc_count, failures;
static _Thread_local size_t last_failure_size,last_failure_alignment;
enum { TRACE_LIMIT=8192 };
static struct { uint64_t offset,bytes,alignment; unsigned op,commit; } trace[TRACE_LIMIT];
static size_t trace_count;
static bool trace_frozen,trace_truncated,trace_dumped;
static void trace_event(unsigned op,size_t offset,size_t bytes,size_t alignment,bool commit) {
    if(trace_frozen)return;
    if(trace_count<TRACE_LIMIT) {
        trace[trace_count].offset=offset;trace[trace_count].bytes=bytes;
        trace[trace_count].alignment=alignment;trace[trace_count].op=op;
        trace[trace_count].commit=commit;trace_count++;
    } else trace_truncated=true;
    if(op==2)trace_frozen=true;
}
static bool bit(const uint64_t* map,size_t i) { return (map[i/64]>>(i%64))&1; }
static void set(uint64_t* map,size_t i,bool value) {
    if (value) map[i/64]|=UINT64_C(1)<<(i%64);
    else map[i/64]&=~(UINT64_C(1)<<(i%64));
}
static bool range(const void* p,size_t n,size_t* first,size_t* count) {
    uintptr_t a=(uintptr_t)p;
    if (!pool || !n || (a%PAGE) || (n%PAGE) || a<pool ||
        a-pool>pages*PAGE || n>pages*PAGE-(a-pool)) return false;
    *first=(a-pool)/PAGE;*count=n/PAGE;return true;
}
static bool all_owned(size_t first,size_t count) {
    for(size_t i=first;i<first+count;i++) if(!bit(allocated,i)) return false;
    return true;
}
/* Kernel permission changes require uniform old permissions. On failure
 * restore completed runs before returning; the metadata isn't changed yet. */
static int permit(size_t first,size_t count,bool value) {
    size_t end=first+count;
    for(size_t i=first;i<end;) {
        bool old=bit(accessible,i);
        size_t next=i+1;
        while(next<end && bit(accessible,next)==old) next++;
        if(old!=value) {
            int err=permissions((void*)(pool+i*PAGE),(next-i)*PAGE,value);
            if(err) {
                for(size_t j=first;j<i;) {
                    bool restore=bit(accessible,j);
                    size_t k=j+1;
                    while(k<i && bit(accessible,k)==restore) k++;
                    if(restore!=value && permissions((void*)(pool+j*PAGE),(k-j)*PAGE,restore)) {
#ifdef MINESWITCH_MIMALLOC_HOST
                        __builtin_trap();
#else
                        diagAbortWithResult(MAKERESULT(Module_Libnx,LibnxError_BadInput));
#endif
                    }
                    j=k;
                }
                return err;
            }
        }
        i=next;
    }
    for(size_t i=first;i<end;i++) {
        bool old=bit(accessible,i);
        if(bit(allocated,i) && old!=value) {
            if(value) access_pages++;else access_pages--;
        }
        set(accessible,i,value);
    }
    return 0;
}
int nxmi_pool_init(void* base,size_t bytes) {
    uintptr_t start=(uintptr_t)base;
    if(!start || start%PAGE || !bytes || bytes%PAGE || bytes/PAGE>MAX_PAGES || start>UINTPTR_MAX-bytes)
        return EINVAL;
    LOCK();
    if(pool) { int rc=pool==start && pages==bytes/PAGE?0:EBUSY;UNLOCK();return rc; }
    pool=start;pages=bytes/PAGE;
    // The loader supplied a physically backed read/write heap.
    memset(accessible,0xff,sizeof(accessible));
    UNLOCK();return 0;
}
/* All requests prefer the smallest suitable extent, preserving large holes
 * when small metadata allocations can use existing gaps. Ties/placement favor
 * high addresses for large requests and low addresses for small requests.
 * All space remains shared; there is no fixed capacity partition. */
static size_t best_fit(size_t need,size_t alignment,bool high) {
    size_t selected=pages,best_span=SIZE_MAX,end=pages;
    while(end) {
        while(end && bit(allocated,end-1)) {
            if(end%64==0 && allocated[end/64-1]==UINT64_MAX)end-=64;else end--;
        }
        size_t run_end=end;
        while(end && !bit(allocated,end-1)) {
            if(end%64==0 && allocated[end/64-1]==0)end-=64;else end--;
        }
        size_t span=run_end-end;
        if(span<need || span>best_span)continue;
        uintptr_t address;
        if(high)address=(pool+(run_end-need)*PAGE)&~(uintptr_t)(alignment-1);
        else {
            uintptr_t begin=pool+end*PAGE;
            if(begin>UINTPTR_MAX-(alignment-1))continue;
            address=(begin+alignment-1)&~(uintptr_t)(alignment-1);
        }
        if(address<pool+end*PAGE || address>pool+(run_end-need)*PAGE)continue;
        size_t candidate=(address-pool)/PAGE;
        if(span==best_span && (high?candidate<selected:candidate>selected))continue;
        selected=candidate;best_span=span;
        if(span==need && high)break;
    }
    return selected;
}
static int take(size_t first,size_t need,size_t alignment,bool commit,void** out) {
    int err=permit(first,need,commit);
    if(err) {
        failures++;last_failure_size=need*PAGE;last_failure_alignment=alignment;
        trace_event(2,SIZE_MAX,need*PAGE,alignment,commit);return err;
    }
    for(size_t k=first;k<first+need;k++)set(allocated,k,true);
    reserved_pages+=need;if(commit)access_pages+=need;
    if(reserved_pages>peak_pages)peak_pages=reserved_pages;
    alloc_count++;*out=(void*)(pool+first*PAGE);
    trace_event(1,first*PAGE,need*PAGE,alignment,commit);return 0;
}
int nxmi_pool_alloc(size_t bytes,size_t alignment,bool commit,void** out) {
    *out=NULL;
    if(!bytes || bytes%PAGE || !alignment || (alignment&(alignment-1))) return EINVAL;
    if(alignment<PAGE) alignment=PAGE;
    size_t need=bytes/PAGE;
    LOCK();
    if(pool && need<=pages) {
        size_t i=best_fit(need,alignment,bytes>1024*1024);
        if(i<pages) {int err=take(i,need,alignment,commit,out);UNLOCK();return err;}
    }
    failures++;last_failure_size=bytes;last_failure_alignment=alignment;
    trace_event(2,SIZE_MAX,bytes,alignment,commit);UNLOCK();return ENOMEM;
}
int nxmi_pool_free(void* base,size_t bytes) {
    LOCK();size_t first,count;
    if(!range(base,bytes,&first,&count) || !all_owned(first,count)) {UNLOCK();return EINVAL;}
    int err=permit(first,count,true);
    if(!err) {
        trace_event(3,first*PAGE,bytes,0,false);
        for(size_t i=first;i<first+count;i++)set(allocated,i,false);
        reserved_pages-=count;access_pages-=count;
    }
    UNLOCK();return err;
}
int nxmi_pool_alloc_some(size_t maximum,size_t minimum,size_t alignment,void** out,size_t* actual) {
    *out=NULL;*actual=0;
    if(!minimum || maximum<minimum || maximum%PAGE || !alignment || (alignment&(alignment-1)))return EINVAL;
    if(alignment<PAGE)alignment=PAGE;
    if(minimum>SIZE_MAX-(alignment-1))return EINVAL;
    minimum=(minimum+alignment-1)&~(alignment-1);
    maximum&=~(alignment-1);
    if(maximum<minimum)return EINVAL;
    LOCK();
    if(pool && minimum/PAGE<=pages) {
        // Consume the smallest useful gap first, even if a larger contiguous
        // extent could satisfy maximum. Scatter-capable GPU allocations need
        // not take that large extent away from ordinary contiguous requests.
        size_t first=best_fit(minimum/PAGE,alignment,false);
        if(first<pages) {
            size_t count=0,limit=maximum/PAGE;
            while(count<limit && first+count<pages && !bit(allocated,first+count))count++;
            size_t bytes=(count*PAGE)&~(alignment-1);
            int err=take(first,bytes/PAGE,alignment,true,out);
            if(!err)*actual=bytes;
            UNLOCK();return err;
        }
    }
    failures++;last_failure_size=minimum;last_failure_alignment=alignment;
    trace_event(2,SIZE_MAX,minimum,alignment,true);UNLOCK();return ENOMEM;
}
void nxmi_pool_dump_trace(void) {
    LOCK();
    if(!trace_frozen || trace_dumped) {UNLOCK();return;}
    trace_dumped=true;size_t count=trace_count;bool complete=!trace_truncated;
    UNLOCK();
    // Frozen records are immutable. Output happens outside the allocator lock.
    printf("MINESWITCH_POOL_TRACE_BEGIN base=0x%" PRIxPTR " capacity=%zu count=%zu complete=%u\n",
           pool,pages*PAGE,count,complete);
    for(size_t i=0;i<count;i++)
        printf("MINESWITCH_POOL_TRACE seq=%zu op=%u offset=%" PRIu64 " bytes=%" PRIu64
               " alignment=%" PRIu64 " commit=%u\n",i,trace[i].op,trace[i].offset,
               trace[i].bytes,trace[i].alignment,trace[i].commit);
    puts("MINESWITCH_POOL_TRACE_END");
}
int nxmi_pool_permission(void* base,size_t bytes,bool access) {
    LOCK();size_t first,count;
    int err=(!range(base,bytes,&first,&count) || !all_owned(first,count))?EINVAL:permit(first,count,access);
    UNLOCK();return err;
}
int nxmi_pool_zero(void* base,size_t bytes) {
    LOCK();size_t first,count;
    if(!range(base,bytes,&first,&count) || !all_owned(first,count)) {UNLOCK();return EINVAL;}
    for(size_t i=first;i<first+count;i++) if(!bit(accessible,i)) {UNLOCK();return EACCES;}
    memset(base,0,bytes);UNLOCK();return 0;
}
bool nxmi_pool_owns(const void* p,size_t bytes) {
    uintptr_t a=(uintptr_t)p;
    LOCK();bool yes=pool && a>=pool && a-pool<pages*PAGE && bytes<=pages*PAGE-(a-pool);
    UNLOCK();return yes;
}
void nxmi_pool_stats(struct nxmi_pool_stats* out,bool scan_largest) {
    LOCK();
    *out=(struct nxmi_pool_stats){.capacity=pages*PAGE,.reserved=reserved_pages*PAGE,
        .accessible=access_pages*PAGE,.peak=peak_pages*PAGE,.allocations=alloc_count,.failures=failures,
        .last_failure_size=last_failure_size,.last_failure_alignment=last_failure_alignment};
    if(scan_largest) {
        size_t run=0;
        for(size_t i=0;i<pages;i++) {
            if(bit(allocated,i))run=0;else if(++run>out->largest_free)out->largest_free=run;
        }
        out->largest_free*=PAGE;
    }
    UNLOCK();
}
