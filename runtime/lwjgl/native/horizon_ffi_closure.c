/* libffi executable closures in shared, documented libnx CodeMemory slabs. */
#include "horizon_ffi_closure.h"
#include <switch.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SLAB_BYTES (64u*1024u)
#define SLOT_ALIGNMENT 64u

typedef struct ClosureSlab {
    Jit memory;
    size_t stride,slots,live;
    bool usable;
    struct ClosureSlab* next;
    uint64_t used[];
} ClosureSlab;
typedef struct ClosureAllocation {
    ClosureSlab* slab;
    size_t slot,bytes;
    struct ClosureAllocation* next;
} ClosureAllocation;
static Mutex closure_mutex;
static ClosureSlab* slabs;
static ClosureAllocation* closures;
static size_t slab_count,live_count;

static void* data_address(const ClosureAllocation* allocation) {
    return (char*)jitGetRwAddr(&allocation->slab->memory)+allocation->slot*allocation->slab->stride;
}
static void* code_address(const ClosureAllocation* allocation) {
    return (char*)jitGetRxAddr(&allocation->slab->memory)+allocation->slot*allocation->slab->stride;
}
static size_t free_slot(ClosureSlab* slab) {
    for (size_t i=0;i<slab->slots;i++)
        if (!(slab->used[i/64]&(UINT64_C(1)<<(i%64)))) return i;
    return SIZE_MAX;
}
static ClosureSlab* create_slab(size_t stride,Result* failure) {
    size_t bytes=stride>SLAB_BYTES?(stride+4095)&~(size_t)4095:SLAB_BYTES;
    size_t slots=bytes/stride,words=(slots+63)/64;
    ClosureSlab* slab=calloc(1,sizeof(*slab)+words*sizeof(uint64_t));
    if (!slab) { *failure=MAKERESULT(Module_Libnx,LibnxError_OutOfMemory); return NULL; }
    Result result=jitCreate(&slab->memory,bytes);
    if (R_FAILED(result)) { *failure=result; free(slab); return NULL; }
    if (slab->memory.type!=JitType_CodeMemory) {
        jitClose(&slab->memory); free(slab);
        *failure=MAKERESULT(Module_Libnx,LibnxError_BadInput); return NULL;
    }
    slab->stride=stride; slab->slots=slots; slab->usable=true;
    slab->next=slabs; slabs=slab; ++slab_count;
    fprintf(stderr,"[LWJGL] CodeMemory slab created bytes=%zu stride=%zu slots=%zu pools=%zu live=%zu\n",
        bytes,stride,slots,slab_count,live_count);
    return slab;
}
void* ffi_closure_alloc(size_t size,void** code) {
    if (!code || size<sizeof(ffi_closure) || size>SIZE_MAX-4095) { errno=EINVAL; return NULL; }
    *code=NULL;
    ClosureAllocation* allocation=calloc(1,sizeof(*allocation));
    if (!allocation) return NULL;
    size_t stride=(size+SLOT_ALIGNMENT-1)&~(size_t)(SLOT_ALIGNMENT-1);
    mutexLock(&closure_mutex);
    ClosureSlab* slab=NULL; size_t slot=SIZE_MAX;
    for (ClosureSlab* candidate=slabs;candidate;candidate=candidate->next) {
        if (candidate->usable && candidate->stride>=stride && candidate->live<candidate->slots) {
            size_t available=free_slot(candidate);
            if (available!=SIZE_MAX) { slab=candidate; slot=available; break; }
        }
    }
    if (!slab) {
        Result result=0;
        slab=create_slab(stride,&result);
        if (!slab) {
            size_t pools=slab_count,live=live_count;
            mutexUnlock(&closure_mutex); free(allocation);
            fprintf(stderr,"[LWJGL] CodeMemory slab create failed=0x%08x pools=%zu live=%zu requested=%zu\n",
                result,pools,live,size);
            errno=R_VALUE(result)==KERNELRESULT(ResourceExhausted)?ENOSPC:ENOMEM;
            return NULL;
        }
        slot=0;
    }
    allocation->slab=slab; allocation->slot=slot; allocation->bytes=size;
    slab->used[slot/64]|=UINT64_C(1)<<(slot%64); ++slab->live; ++live_count;
    allocation->next=closures; closures=allocation;
    void* data=data_address(allocation);
    memset(data,0,slab->stride);
    *code=code_address(allocation);
    mutexUnlock(&closure_mutex);
    return data;
}
void* ffi_data_to_code_pointer(void* data) {
    void* result=NULL;
    mutexLock(&closure_mutex);
    for (ClosureAllocation* allocation=closures;allocation;allocation=allocation->next) {
        uintptr_t base=(uintptr_t)data_address(allocation),value=(uintptr_t)data;
        if (value>=base && value-base<allocation->bytes) {
            result=(char*)code_address(allocation)+(value-base); break;
        }
    }
    mutexUnlock(&closure_mutex);
    return result;
}
ffi_status horizon_ffi_publish_closure(ffi_closure* closure) {
    Result result=MAKERESULT(Module_Libnx,LibnxError_BadInput);
    mutexLock(&closure_mutex);
    for (ClosureAllocation* allocation=closures;allocation;allocation=allocation->next) {
        if (data_address(allocation)==closure) {
            // CodeMemory retains readable writable and executable aliases.
            // Publishing another slot flushes the shared backing; it never
            // revokes permissions or changes bytes of existing live closures.
            result=jitTransitionToExecutable(&allocation->slab->memory); break;
        }
    }
    mutexUnlock(&closure_mutex);
    if (R_FAILED(result)) {
        fprintf(stderr,"[LWJGL] Horizon closure publish failed: 0x%08x\n",result);
        return FFI_BAD_TYPEDEF;
    }
    return FFI_OK;
}
void horizon_ffi_closure_counts(size_t* live,size_t* pools) {
    mutexLock(&closure_mutex);
    *live=live_count; *pools=slab_count;
    mutexUnlock(&closure_mutex);
}
void ffi_closure_free(void* pointer) {
    if (!pointer) return;
    mutexLock(&closure_mutex);
    ClosureAllocation** link=&closures;
    while (*link && data_address(*link)!=pointer) link=&(*link)->next;
    ClosureAllocation* allocation=*link;
    if (!allocation) {
        mutexUnlock(&closure_mutex);
        fprintf(stderr,"[LWJGL] Rejecting unknown Horizon closure free\n"); errno=EINVAL; return;
    }
    ClosureSlab* slab=allocation->slab;
    if (slab->live==1) {
        // Only the last allocation can release its slab. Other live callbacks
        // must never lose their shared RW/RX mappings when a peer is freed.
        Result result=jitClose(&slab->memory);
        if (R_FAILED(result)) {
            // An unmap may have partially succeeded. Quarantine the mapping;
            // no later allocation may reuse a possibly inaccessible RW alias.
            slab->usable=false;
            mutexUnlock(&closure_mutex);
            fprintf(stderr,"[LWJGL] Horizon empty-slab close failed: 0x%08x\n",result);
            errno=EIO; return;
        }
        ClosureSlab** slab_link=&slabs;
        while (*slab_link!=slab) slab_link=&(*slab_link)->next;
        *slab_link=slab->next; --slab_count;
    } else {
        slab->used[allocation->slot/64]&=~(UINT64_C(1)<<(allocation->slot%64));
    }
    *link=allocation->next; --slab->live; --live_count;
    bool empty=slab->live==0;
    if (empty) fprintf(stderr,"[LWJGL] CodeMemory slab released pools=%zu live=%zu\n",slab_count,live_count);
    mutexUnlock(&closure_mutex);
    if (empty) free(slab);
    free(allocation);
}
