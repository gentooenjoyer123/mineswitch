/* Included by mimalloc's primitive dispatcher. Real fixed backing and libnx
 * permissions; no mmap emulation, overcommit or pretend memory release. */
#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"
#include "page_pool.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#ifdef MINESWITCH_MIMALLOC_HOST
#include <pthread.h>
#include <sys/random.h>
#include <time.h>
static pthread_key_t nxmi_thread_key;
#else
#include <switch.h>
static s32 nxmi_thread_key=-1;
extern char* fake_heap_start;
extern char* fake_heap_end;
#endif

void _mi_prim_mem_init(mi_os_mem_config_t* config) {
#ifndef MINESWITCH_MIMALLOC_HOST
    int result=nxmi_pool_init(fake_heap_start,(uintptr_t)fake_heap_end-(uintptr_t)fake_heap_start);
    if(result) diagAbortWithResult(MAKERESULT(Module_Libnx,LibnxError_HeapAllocFailed));
#endif
    struct nxmi_pool_stats stats;nxmi_pool_stats(&stats,false);
    config->page_size=4096;config->large_page_size=0;config->alloc_granularity=4096;
    config->physical_memory_in_kib=stats.capacity/1024;
    config->has_overcommit=false;config->has_virtual_reserve=false;config->has_partial_free=true;
}
int _mi_prim_alloc(void* hint,size_t size,size_t align,bool commit,bool large,bool* is_large,bool* zero,void** out) {
    (void)hint;(void)large;
    *is_large=false;*zero=false;
    return nxmi_pool_alloc(size,align,commit,out);
}
int _mi_prim_free(void* p,size_t n) { return nxmi_pool_free(p,n); }
int _mi_prim_commit(void* p,size_t n,bool* zero) {
    *zero=false;return nxmi_pool_permission(p,n,true);
}
int _mi_prim_decommit(void* p,size_t n,bool* recommit) {
    *recommit=true;return nxmi_pool_permission(p,n,false);
}
int _mi_prim_reset(void* p,size_t n) { return nxmi_pool_zero(p,n); }
int _mi_prim_reuse(void* p,size_t n) { return nxmi_pool_owns(p,n)?0:EINVAL; }
int _mi_prim_protect(void* p,size_t n,bool protect) { return nxmi_pool_permission(p,n,!protect); }
int _mi_prim_alloc_huge_os_pages(void* hint,size_t n,int numa,bool* zero,void** out) {
    (void)hint;(void)n;(void)numa;*zero=false;*out=NULL;return ENOTSUP;
}
size_t _mi_prim_numa_node(void) { return 0; }
size_t _mi_prim_numa_node_count(void) { return 1; }
mi_msecs_t _mi_prim_clock_now(void) {
#ifdef MINESWITCH_MIMALLOC_HOST
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (mi_msecs_t)now.tv_sec*1000+now.tv_nsec/1000000;
#else
    return armTicksToNs(armGetSystemTick())/1000000;
#endif
}
void _mi_prim_process_info(mi_process_info_t* out) {
#ifndef MINESWITCH_MIMALLOC_HOST
    u64 value=0;
    if(R_SUCCEEDED(svcGetInfo(&value,InfoType_UsedMemorySize,CUR_PROCESS_HANDLE,0))) {
        out->current_rss=value;out->current_commit=value;
    }
#else
    (void)out; // Keep mimalloc's allocator-based defaults in the host test.
#endif
}
void _mi_prim_out_stderr(const char* text) {
    // Raw descriptor IO avoids allocating a stdio buffer during initialization.
    (void)write(STDERR_FILENO,text,strlen(text));
}
bool _mi_prim_getenv(const char* name,char* result,size_t capacity) {
    if(_mi_preloading()) return false;
    const char* value=getenv(name);
    if(!value || strnlen(value,capacity)>=capacity)return false;
    memcpy(result,value,strlen(value)+1);return true;
}
bool _mi_prim_random_buf(void* p,size_t n) {
#ifdef MINESWITCH_MIMALLOC_HOST
    size_t done=0;
    while(done<n) {
        ssize_t got=getrandom((char*)p+done,n-done,0);
        if(got<0 && errno==EINTR)continue;
        if(got<=0)return false;
        done+=(size_t)got;
    }
#else
    randomGet(p,n);
#endif
    return true;
}
static void nxmi_thread_finished(void* heap) { _mi_thread_done((mi_heap_t*)heap); }
void _mi_prim_thread_init_auto_done(void) {
#ifdef MINESWITCH_MIMALLOC_HOST
    if(pthread_key_create(&nxmi_thread_key,nxmi_thread_finished))__builtin_trap();
#else
    nxmi_thread_key=threadTlsAlloc(nxmi_thread_finished);
    if(nxmi_thread_key<0)diagAbortWithResult(MAKERESULT(Module_Libnx,LibnxError_OutOfMemory));
#endif
}
void _mi_prim_thread_done_auto_done(void) {
    // Process exit may still have live threads. Do not invalidate their key.
}
void _mi_prim_thread_associate_default_heap(mi_heap_t* heap) {
#ifdef MINESWITCH_MIMALLOC_HOST
    if(pthread_setspecific(nxmi_thread_key,heap))__builtin_trap();
#else
    if(nxmi_thread_key>=0)threadTlsSet(nxmi_thread_key,heap);
#endif
}

#define MI_PRIM_HAS_PROCESS_ATTACH 1
#ifndef MINESWITCH_MIMALLOC_HOST
static void __attribute__((constructor(101))) nxmi_process_start(void) {
    _mi_auto_process_init();
}
// No process-wide destructor while VM/driver threads can still be running.
#endif
