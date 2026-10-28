/* Diagnostic-only allocation inventory. Never changes allocation requests. */
#include "alloc_trace.h"
#include <switch.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef MINESWITCH_ALLOCATOR_MIMALLOC
#include "../memory/mimalloc/newlib_adapter.h"
#include "../memory/mimalloc/page_pool.h"
#endif
#ifdef MINESWITCH_NXVK_RECLAIM
#include "../graphics/nxvk/cache_pressure.h"
#endif
#if !defined(MINESWITCH_ALLOC_TRACE_HOST) && !defined(MINESWITCH_ALLOCATOR_MIMALLOC)
#include <reent.h>
#include "../memory/newlib/free_snapshot.h"
#endif
#ifdef MINESWITCH_ALLOC_TRACE_NOUVEAU
#include <nouveau.h>
#endif

#define SLOTS 4096u
#define MIN_TRACK (256u*1024u)
typedef struct { uintptr_t pointer, caller; size_t bytes; } Allocation;
static Allocation live[SLOTS];
static Mutex trace_lock;
static atomic_bool enabled;
static _Thread_local unsigned depth;
static size_t live_bytes,peak_bytes,dropped;
static unsigned events;

#if !defined(MINESWITCH_ALLOC_TRACE_HOST) && !defined(MINESWITCH_ALLOCATOR_MIMALLOC)
static atomic_uint_fast64_t snapshot_id;
static void report_free_blocks(const char* scope,const char* capture,
                               const char* kind,size_t outer_bytes,
                               const struct nxalloc_snapshot* s) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    size_t capacity=(uintptr_t)fake_heap_end-(uintptr_t)fake_heap_start;
    uint64_t id=atomic_fetch_add_explicit(&snapshot_id,1,memory_order_relaxed)+1;
    printf("MINESWITCH_FREE_BLOCKS id=%" PRIu64 " scope=%s capture=%s kind=%s outer_bytes=%zu"
           " requested=%zu padded=%zu arena=%zu free=%zu count=%zu largest_chunk=%zu"
           " largest_request=%zu fitting=%zu top=%zu unclaimed=%zu invalid=%u truncated=%u\n",
           id,scope,capture,kind,outer_bytes,s->requested,s->padded,s->arena_bytes,s->free_bytes,
           s->chunks,s->largest_chunk,s->largest_request,s->fitting_chunks,s->top_bytes,
           capacity>s->arena_bytes?capacity-s->arena_bytes:0,s->invalid,s->truncated);
    const char* names[NXALLOC_BUCKETS]={"lt4k","lt64k","lt256k","lt1m","lt2m","lt4m","lt16m","lt64m","ge64m"};
    char line[768];
    size_t used=(size_t)snprintf(line,sizeof(line),"MINESWITCH_FREE_HISTOGRAM id=%" PRIu64,id);
    for (unsigned i=0;i<NXALLOC_BUCKETS && used<sizeof(line);i++) {
        int n=snprintf(line+used,sizeof(line)-used," %s_count=%zu %s_bytes=%zu",names[i],s->count[i],names[i],s->bytes[i]);
        if (n<0 || (size_t)n>=sizeof(line)-used) break;
        used+=(size_t)n;
    }
    puts(line);
}
static void report_failed_blocks(const char* kind,size_t bytes) {
    struct nxalloc_snapshot snap;
    int captured=mineswitch_newlib_failure_take(&snap);
    if (!captured) mineswitch_newlib_snapshot(_REENT,&snap);
    report_free_blocks("failure",captured?"at_failure_locked":"after_failure_locked",kind,bytes,&snap);
}
#endif

static unsigned enter(void) {
    if (!atomic_load_explicit(&enabled,memory_order_relaxed)) return 0;
    unsigned token=++depth==1?2:1;
#if !defined(MINESWITCH_ALLOC_TRACE_HOST) && !defined(MINESWITCH_ALLOCATOR_MIMALLOC)
    if (token==2) mineswitch_newlib_failure_reset();
#endif
    return token;
}
static size_t slot(uintptr_t pointer,bool inserting) {
    size_t index=((pointer>>4)^(pointer>>19))&(SLOTS-1), tomb=SLOTS;
    for (size_t i=0;i<SLOTS;i++,index=(index+1)&(SLOTS-1)) {
        if (live[index].pointer==pointer) return index;
        if (live[index].pointer==1 && tomb==SLOTS) tomb=index;
        if (!live[index].pointer) return inserting?(tomb==SLOTS?index:tomb):SLOTS;
    }
    return inserting?tomb:SLOTS;
}
static Allocation untrack(uintptr_t pointer) {
    Allocation saved={0};
    if (!pointer) return saved;
    mutexLock(&trace_lock);
    size_t at=slot(pointer,false);
    if (at<SLOTS) { saved=live[at];live_bytes-=saved.bytes;live[at]=(Allocation){.pointer=1}; }
    mutexUnlock(&trace_lock);
    return saved;
}
static void restore(Allocation saved) {
    if (!saved.pointer) return;
    mutexLock(&trace_lock);
    size_t at=slot(saved.pointer,true);
    if (at<SLOTS) { live[at]=saved;live_bytes+=saved.bytes; }
    else ++dropped;
    mutexUnlock(&trace_lock);
}
static void record(const char* kind,uintptr_t old,void* result,size_t bytes,
                   uintptr_t caller,bool success,unsigned token) {
    int saved_errno=errno;
    if (token==2) {
#ifdef MINESWITCH_ALLOCATOR_MIMALLOC
        if (!success && bytes) {nxmi_report();nxmi_pool_dump_trace();}
#elif !defined(MINESWITCH_ALLOC_TRACE_HOST)
        if (!success && bytes) report_failed_blocks(kind,bytes);
#endif
        mutexLock(&trace_lock);
        if (old && success) {
            size_t at=slot(old,false);
            if (at<SLOTS) { live_bytes-=live[at].bytes;live[at]=(Allocation){.pointer=1}; }
        }
        if (result && bytes>=MIN_TRACK) {
            size_t at=slot((uintptr_t)result,true);
            if (at<SLOTS) {
                if (live[at].pointer>1) live_bytes-=live[at].bytes;
                live[at]=(Allocation){(uintptr_t)result,caller,bytes};live_bytes+=bytes;
                if (live_bytes>peak_bytes) peak_bytes=live_bytes;
            } else ++dropped;
        }
        bool print=(!success && bytes) || (bytes>=1024u*1024u && events<256);
        if (print) ++events;
        mutexUnlock(&trace_lock);
        if (print) printf("MINESWITCH_ALLOC kind=%s bytes=%zu pointer=%p old=0x%" PRIxPTR
            " caller=0x%" PRIxPTR " success=%d errno=%d\n",kind,bytes,result,old,caller,success,saved_errno);
    }
    if (token) --depth;
    errno=saved_errno;
}
void* __real_malloc(size_t);
void* __real_calloc(size_t,size_t);
void* __real_realloc(void*,size_t);
void* __real_memalign(size_t,size_t);
int __real_posix_memalign(void**,size_t,size_t);
void __real_free(void*);
#ifdef MINESWITCH_NXVK_RECLAIM
static size_t reclaim_outer(unsigned token,size_t bytes,int error) {
    if(token!=2 || !bytes || bytes>(size_t)PTRDIFF_MAX || error!=ENOMEM)return 0;
    // All libc operations have returned and released their own locks. The
    // registry and driver use try-locks and guard recursive reclamation.
    --depth;size_t released=mineswitch_nxvk_reclaim_caches();++depth;
    return released;
}
static void report_retry(const char* kind,size_t bytes,size_t released,bool success) {
    int saved_errno=errno;
    printf("MINESWITCH_NATIVE_RECLAIM kind=%s requested=%zu released=%zu retry_success=%d\n",
           kind,bytes,released,success);
    errno=saved_errno;
}
#endif
void* __wrap_malloc(size_t n) {
    unsigned token=enter();void* p=__real_malloc(n);
#ifdef MINESWITCH_NXVK_RECLAIM
    size_t released=p?0:reclaim_outer(token,n,errno);
    if(released) {p=__real_malloc(n);report_retry("malloc",n,released,p!=NULL);}
#endif
    record("malloc",0,p,n,(uintptr_t)__builtin_return_address(0),p||!n,token);return p;
}
void* __wrap_calloc(size_t count,size_t size) {
    unsigned token=enter();void* p=__real_calloc(count,size);
    size_t n=size && count>SIZE_MAX/size?SIZE_MAX:count*size;
#ifdef MINESWITCH_NXVK_RECLAIM
    size_t released=p?0:reclaim_outer(token,n,errno);
    if(released) {p=__real_calloc(count,size);report_retry("calloc",n,released,p!=NULL);}
#endif
    record("calloc",0,p,n,(uintptr_t)__builtin_return_address(0),p||!n,token);return p;
}
void* __wrap_realloc(void* old,size_t n) {
    uintptr_t old_address=(uintptr_t)old;
    unsigned token=enter();
    // Retire before libc can return the old address to another thread. Restore
    // only on failed growth, which leaves ownership with this caller.
    Allocation saved=token==2?untrack(old_address):(Allocation){0};
    void* p=__real_realloc(old,n);
    int saved_errno=errno;
#ifdef MINESWITCH_NXVK_RECLAIM
    // Only an outer, failed public realloc: libc has released its lock and
    // still owns the original buffer. Never shrink or substitute the request.
    if (token==2 && !p && n && n<=(size_t)PTRDIFF_MAX && saved_errno==ENOMEM) {
#if !defined(MINESWITCH_ALLOC_TRACE_HOST) && !defined(MINESWITCH_ALLOCATOR_MIMALLOC)
        report_failed_blocks("realloc_before_reclaim",n);
#endif
        // The callback frees separate cached allocations. Track those frees
        // normally; the registry itself guards reentrant reclamation.
        size_t released=reclaim_outer(token,n,saved_errno);
        if (released) {
            errno=saved_errno;
            p=__real_realloc(old,n); // One retry; NULL still preserves old.
            saved_errno=errno;
            report_retry("realloc",n,released,p!=NULL);
        }
    }
#endif
    if (token==2 && !p && n) restore(saved);
    errno=saved_errno;
    record("realloc",0,p,n,(uintptr_t)__builtin_return_address(0),p||!n,token);return p;
}
void* __wrap_memalign(size_t alignment,size_t n) {
    unsigned token=enter();void* p=__real_memalign(alignment,n);
#ifdef MINESWITCH_NXVK_RECLAIM
    size_t released=p?0:reclaim_outer(token,n,errno);
    if(released) {p=__real_memalign(alignment,n);report_retry("memalign",n,released,p!=NULL);}
#endif
    record("memalign",0,p,n,(uintptr_t)__builtin_return_address(0),p||!n,token);return p;
}
int __wrap_posix_memalign(void** out,size_t alignment,size_t n) {
    unsigned token=enter();int rc=__real_posix_memalign(out,alignment,n);
#ifdef MINESWITCH_NXVK_RECLAIM
    size_t released=rc?reclaim_outer(token,n,rc):0;
    if(released) {rc=__real_posix_memalign(out,alignment,n);report_retry("posix_memalign",n,released,rc==0);}
#endif
    record("posix_memalign",0,rc?NULL:*out,n,(uintptr_t)__builtin_return_address(0),rc==0,token);return rc;
}
void __wrap_free(void* p) {
    uintptr_t old=(uintptr_t)p;
    unsigned token=enter();
    if (token==2) (void)untrack(old);
    __real_free(p);
    record("free",0,NULL,0,(uintptr_t)__builtin_return_address(0),true,token);
}
#ifdef MINESWITCH_ALLOC_TRACE_NOUVEAU
int __real_nouveau_bo_new(struct nouveau_device*,uint32_t,uint32_t,uint64_t,union nouveau_bo_config*,struct nouveau_bo**);
int __wrap_nouveau_bo_new(struct nouveau_device* device,uint32_t flags,uint32_t alignment,
                         uint64_t bytes,union nouveau_bo_config* config,struct nouveau_bo** out) {
    int rc=__real_nouveau_bo_new(device,flags,alignment,bytes,config,out);
    int saved_errno=errno;
    if (atomic_load_explicit(&enabled,memory_order_relaxed) && (bytes>=1024u*1024u || rc)) {
        ++depth;
        static atomic_flag device_announced=ATOMIC_FLAG_INIT;
        if (!atomic_flag_test_and_set_explicit(&device_announced,memory_order_relaxed)) {
            const nvioctl_gpu_characteristics* gpu=nvGpuGetCharacteristics();
            u32 count=0;const u32* masks=nvGpuGetTpcMasks(&count);
            printf("MINESWITCH_GPU_DEVICE chipset=0x%x drm_version=0x%x gpc=%u tpc_per_gpc=%u gpc_mask=0x%x masks=%u\n",
                device->chipset,nouveau_drm(&device->object)->version,gpu->num_gpc,gpu->num_tpc_per_gpc,gpu->gpc_mask,count);
            for (u32 i=0;masks && i<count && i<8;i++) printf("MINESWITCH_GPU_TPC_MASK index=%u mask=0x%x\n",i,masks[i]);
        }
        printf("MINESWITCH_GPU_BO bytes=%" PRIu64 " flags=0x%08x alignment=%u kind=0x%08x "
               "caller=0x%" PRIxPTR " success=%d rc=%d\n",bytes,flags,alignment,
               config?config->nvc0.memtype:0,(uintptr_t)__builtin_return_address(0),rc==0,rc);
        --depth;
    }
    errno=saved_errno;
    return rc;
}
#endif
void allocator_trace_start(void) {
    printf("MINESWITCH_ALLOC_TRACE reporter=%p threshold=%u slots=%u\n",allocator_trace_snapshot,MIN_TRACK,SLOTS);
    const InfoType types[]={InfoType_HeapRegionAddress,InfoType_HeapRegionSize,
        InfoType_AliasRegionAddress,InfoType_AliasRegionSize,InfoType_StackRegionAddress,
        InfoType_StackRegionSize,InfoType_AslrRegionAddress,InfoType_AslrRegionSize,
        InfoType_SystemResourceSizeTotal};
    for (size_t i=0;i<sizeof(types)/sizeof(types[0]);i++) {
        u64 value=0;Result rc=svcGetInfo(&value,types[i],CUR_PROCESS_HANDLE,0);
        printf("MINESWITCH_REGION type=%u value=0x%" PRIx64 " rc=0x%08x\n",(unsigned)types[i],value,rc);
    }
    atomic_store_explicit(&enabled,true,memory_order_relaxed);
}
void allocator_trace_snapshot(void) {
    if (!atomic_load_explicit(&enabled,memory_order_relaxed)) return;
    ++depth;
    Allocation largest[8]={0},top_owners[12]={0};
    // Scratch table lives in BSS; no recursive allocation or large stack use.
    static Allocation owners[SLOTS];
    size_t total,peak,lost,count=0;
    mutexLock(&trace_lock);
    memset(owners,0,sizeof(owners));
    total=live_bytes;peak=peak_bytes;lost=dropped;
    for (size_t i=0;i<SLOTS;i++) if (live[i].pointer>1) {
        ++count;
        size_t owner=((live[i].caller>>2)^(live[i].caller>>17))&(SLOTS-1);
        while (owners[owner].caller && owners[owner].caller!=live[i].caller)
            owner=(owner+1)&(SLOTS-1);
        owners[owner].caller=live[i].caller;
        owners[owner].bytes+=live[i].bytes;
        owners[owner].pointer++; // Count, not an address, in the owner table.
        for (size_t j=0;j<8;j++) if (live[i].bytes>largest[j].bytes) {
            for (size_t k=7;k>j;k--) largest[k]=largest[k-1];
            largest[j]=live[i];break;
        }
    }
    for (size_t i=0;i<SLOTS;i++) if (owners[i].pointer) {
        for (size_t j=0;j<12;j++) if (owners[i].bytes>top_owners[j].bytes) {
            for (size_t k=11;k>j;k--) top_owners[k]=top_owners[k-1];
            top_owners[j]=owners[i];break;
        }
    }
    mutexUnlock(&trace_lock);
    printf("MINESWITCH_ALLOC_SUMMARY bytes=%zu peak=%zu count=%zu dropped=%zu\n",total,peak,count,lost);
    for (size_t i=0;i<8 && largest[i].pointer;i++)
        printf("MINESWITCH_ALLOC_LIVE pointer=0x%" PRIxPTR " bytes=%zu caller=0x%" PRIxPTR "\n",
               largest[i].pointer,largest[i].bytes,largest[i].caller);
    for (size_t i=0;i<12 && top_owners[i].pointer;i++)
        printf("MINESWITCH_ALLOC_OWNER caller=0x%" PRIxPTR " bytes=%zu count=%" PRIuPTR "\n",
               top_owners[i].caller,top_owners[i].bytes,top_owners[i].pointer);
#if !defined(MINESWITCH_ALLOC_TRACE_HOST) && !defined(MINESWITCH_ALLOCATOR_MIMALLOC)
    struct nxalloc_snapshot snap;
    mineswitch_newlib_snapshot(_REENT,&snap);
    report_free_blocks("periodic","locked","none",0,&snap);
#elif defined(MINESWITCH_ALLOCATOR_MIMALLOC)
    nxmi_report();
#endif
    --depth;
}
