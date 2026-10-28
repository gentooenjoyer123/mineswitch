#include <assert.h>
#include <malloc.h>
#include <string.h>
#define MINESWITCH_ALLOC_TRACE_HOST 1
#include "../../alloc_trace.c"
static unsigned fail_malloc,fail_calloc,fail_memalign,fail_posix;
void* __real_malloc(size_t n) { if(fail_malloc){fail_malloc--;errno=ENOMEM;return NULL;} return malloc(n); }
void* __real_calloc(size_t n,size_t s) { if(fail_calloc){fail_calloc--;errno=ENOMEM;return NULL;} return calloc(n,s); }
static unsigned fail_realloc;
void* __real_realloc(void* p,size_t n) {
    if (fail_realloc) { --fail_realloc;errno=ENOMEM;return NULL; }
    return realloc(p,n);
}
void* __real_memalign(size_t a,size_t n) { if(fail_memalign){fail_memalign--;errno=ENOMEM;return NULL;} return memalign(a,n); }
int __real_posix_memalign(void** p,size_t a,size_t n) { if(fail_posix){fail_posix--;return ENOMEM;} return posix_memalign(p,a,n); }
void __real_free(void* p) { free(p); }
#ifdef MINESWITCH_NXVK_RECLAIM
static void* cached;
static unsigned trim_calls;
static size_t trim(void* owner) {
    assert(owner==&cached);
    assert(mineswitch_nxvk_reclaim_caches()==0); // Recursive callback never locks itself.
    trim_calls++;
    if (!cached) return 0;
    __wrap_free(cached);cached=NULL;
    return 8*1024*1024;
}
static void pressure_retry(void) {
    unsigned char* p=__wrap_malloc(512*1024);assert(p);p[0]=0x45;p[512*1024-1]=0x32;
    cached=__wrap_malloc(8*1024*1024);assert(cached);
    assert(mineswitch_nxvk_register_cache(&cached,trim));
    assert(!mineswitch_nxvk_register_cache(&cached,trim));
    fail_realloc=1;
    unsigned char* q=__wrap_realloc(p,1024*1024);
    assert(q && q[0]==0x45 && q[512*1024-1]==0x32 && trim_calls==1);
    assert(live_bytes==1024*1024 && cached==NULL && fail_realloc==0);
    p=q;
    fail_realloc=1;
    assert(__wrap_realloc(p,2*1024*1024)==NULL && trim_calls==2 && live_bytes==1024*1024);
    assert(p[0]==0x45 && p[512*1024-1]==0x32); // Empty cache leaves old owned.
    cached=__wrap_malloc(8*1024*1024);assert(cached);
    fail_realloc=2;
    assert(__wrap_realloc(p,2*1024*1024)==NULL && trim_calls==3 && fail_realloc==0);
    assert(cached==NULL && live_bytes==1024*1024 && p[0]==0x45 && errno==ENOMEM);
    mineswitch_nxvk_unregister_cache(&cached);
    assert(mineswitch_nxvk_reclaim_caches()==0 && trim_calls==3);
    __wrap_free(p);
    assert(live_bytes==0 && depth==0);
    puts("NXVK_REALLOC_RETRY_PASS recovered growth, preserved failed growth, empty cache, tracking, recursion, one retry only");
    assert(mineswitch_nxvk_register_cache(&cached,trim));
    cached=__wrap_malloc(8*1024*1024);fail_malloc=1;
    p=__wrap_malloc(1024*1024);assert(p && !cached && !fail_malloc);__wrap_free(p);
    cached=__wrap_malloc(8*1024*1024);fail_calloc=1;
    p=__wrap_calloc(1024,1024);assert(p && !p[0] && !p[1024*1024-1] && !cached);__wrap_free(p);
    cached=__wrap_malloc(8*1024*1024);fail_memalign=1;
    p=__wrap_memalign(65536,1024*1024);assert(p && (uintptr_t)p%65536==0 && !cached);__wrap_free(p);
    cached=__wrap_malloc(8*1024*1024);fail_posix=1;void* aligned=NULL;
    errno=EDOM;
    assert(__wrap_posix_memalign(&aligned,65536,1024*1024)==0 && errno==EDOM);
    assert(aligned && (uintptr_t)aligned%65536==0 && !cached);__wrap_free(aligned);
    unsigned before=trim_calls;aligned=(void*)0x55;
    assert(__wrap_posix_memalign(&aligned,3,1024)==EINVAL && aligned==(void*)0x55 && trim_calls==before);
    cached=__wrap_malloc(8*1024*1024);fail_malloc=2;
    assert(!__wrap_malloc(1024*1024) && !cached && !fail_malloc && trim_calls==before+1);
    before=trim_calls;
    assert(!__wrap_malloc(SIZE_MAX) && trim_calls==before);
    mineswitch_nxvk_unregister_cache(&cached);
    assert(live_bytes==0 && depth==0);
    puts("NXVK_ALL_ALLOC_RETRY_PASS malloc,calloc,memalign,posix_memalign,zeroing,alignment,errno,invalid/oversized skip,one retry");
}
#endif
static void* worker(void* unused) {
    (void)unused;
    for (unsigned i=0;i<200;i++) {
        unsigned char* p=__wrap_malloc(512*1024);
        assert(p);p[0]=0x71;p[512*1024-1]=0x29;
        p=__wrap_realloc(p,768*1024);
        assert(p && p[0]==0x71 && p[512*1024-1]==0x29);
        __wrap_free(p);
    }
    return NULL;
}
int main(void) {
    assert(pthread_mutex_init(&trace_lock,NULL)==0);
    events=256;
    void* inactive=__wrap_malloc(1024*1024);assert(inactive && live_bytes==0);__wrap_free(inactive);
    atomic_store(&enabled,true);
    unsigned char* p=__wrap_calloc(1024,1024);assert(p && live_bytes==1024*1024 && p[0]==0);
    p[0]=0x42;
    p=__wrap_realloc(p,2*1024*1024);assert(p && p[0]==0x42 && live_bytes==2*1024*1024);
    errno=0;
    assert(__wrap_realloc(p,SIZE_MAX)==NULL && errno==ENOMEM && p[0]==0x42 && live_bytes==2*1024*1024);
    p=__wrap_realloc(p,128);assert(p && live_bytes==0 && p[0]==0x42);__wrap_free(p);
    p=__wrap_memalign(65536,1024*1024);assert(p && (uintptr_t)p%65536==0 && live_bytes==1024*1024);__wrap_free(p);
    void* aligned=NULL;
    assert(__wrap_posix_memalign(&aligned,65536,512*1024)==0 && live_bytes==512*1024);__wrap_free(aligned);
    assert(live_bytes==0 && depth==0);
#ifdef MINESWITCH_NXVK_RECLAIM
    pressure_retry();
#endif
    void* grouped[16];
    for (unsigned i=0;i<16;i++) grouped[i]=__wrap_malloc(512*1024);
    allocator_trace_snapshot();
    assert(live_bytes==8*1024*1024);
    for (unsigned i=0;i<16;i++) __wrap_free(grouped[i]);
    pthread_t threads[4];
    for (unsigned i=0;i<4;i++) assert(pthread_create(&threads[i],NULL,worker,NULL)==0);
    for (unsigned i=0;i<4;i++) assert(pthread_join(threads[i],NULL)==0);
    assert(live_bytes==0 && dropped==0 && depth==0);
    for (unsigned i=0;i<SLOTS;i++) assert(live[i].pointer<=1);
    allocator_trace_snapshot();
    assert(live_bytes==0 && depth==0);
    puts("ALLOC_TRACE_HOST_PASS real allocations, failed-realloc retention, aligned allocations,800 concurrent reuse cycles,zero live records");
    return 0;
}
