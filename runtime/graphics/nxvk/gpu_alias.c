#include "gpu_alias.h"
#include "../../memory/mimalloc/page_pool.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef MINESWITCH_GPU_ALIAS_HOST
#include "tests/alias_platform.h"
static Mutex registry_lock=PTHREAD_MUTEX_INITIALIZER;
#else
#include <switch.h>
static Mutex registry_lock;
#endif

enum { PAGE=4096, MIN_PIECE=65536, MAX_PIECE=2*1024*1024 };
static const size_t virtual_budget=(size_t)1024*1024*1024;
typedef struct { void* source;size_t bytes; } Piece;
typedef struct Alias {
    struct Alias* next;
    void* address;
    VirtmemReservation* reservation;
    size_t bytes,count,mapped;
    Piece pieces[];
} Alias;
static Alias* aliases;
static size_t held,active,allocations,failures,total_pieces;
static Result last_result;

static void release_storage(Alias* a) {
    size_t offset=0;
    for(size_t i=0;i<a->mapped;i++)offset+=a->pieces[i].bytes;
    if(offset)armDCacheFlush(a->address,offset);
    while(a->mapped) {
        Piece* p=&a->pieces[--a->mapped];offset-=p->bytes;
        Result rc=svcUnmapMemory((char*)a->address+offset,p->source,p->bytes);
        // Never recycle physical pages if an alias still exists.
        if(R_FAILED(rc))diagAbortWithResult(rc);
    }
    if(a->reservation) {
        virtmemLock();virtmemRemoveReservation(a->reservation);virtmemUnlock();
    }
    for(size_t i=0;i<a->count;i++) {
        if(nxmi_pool_free(a->pieces[i].source,a->pieces[i].bytes))
            diagAbortWithResult(MAKERESULT(Module_Libnx,LibnxError_BadInput));
    }
}

void* mineswitch_nxvk_alloc_cached(size_t bytes,size_t alignment) {
    if(bytes<1024*1024 || bytes%PAGE || !alignment || (alignment&(alignment-1)) ||
       alignment>MAX_PIECE || bytes>virtual_budget) return NULL;
    if(alignment<PAGE)alignment=PAGE;
    if(bytes%alignment)return NULL;
    size_t min_piece=alignment>MIN_PIECE?alignment:MIN_PIECE;
    size_t capacity=(bytes+min_piece-1)/min_piece;
    if(capacity>(SIZE_MAX-sizeof(Alias))/sizeof(Piece))return NULL;
    Alias* a=calloc(1,sizeof(*a)+capacity*sizeof(Piece));
    if(!a)return NULL;
    a->bytes=bytes;
    mutexLock(&registry_lock);
    if(bytes>virtual_budget-held) {mutexUnlock(&registry_lock);free(a);return NULL;}
    held+=bytes;mutexUnlock(&registry_lock);

    virtmemLock();
    void* found=virtmemFindStack(bytes+alignment,PAGE);
    if(found) {
        a->address=(void*)(((uintptr_t)found+alignment-1)&~(uintptr_t)(alignment-1));
        a->reservation=virtmemAddReservation(a->address,bytes);
    }
    virtmemUnlock();
    Result rc=0;
    if(!a->reservation)goto failed;
    size_t offset=0;
    while(offset<bytes) {
        size_t remaining=bytes-offset;
        size_t maximum=remaining<MAX_PIECE?remaining:MAX_PIECE;
        size_t minimum=remaining<min_piece?remaining:min_piece;
        if(a->count==capacity)goto failed;
        Piece* p=&a->pieces[a->count];
        if(nxmi_pool_alloc_some(maximum,minimum,alignment,&p->source,&p->bytes))goto failed;
        a->count++;
        armDCacheFlush(p->source,p->bytes);
        rc=svcMapMemory((char*)a->address+offset,p->source,p->bytes);
        if(R_FAILED(rc))goto failed;
        a->mapped++;offset+=p->bytes;
    }
    mutexLock(&registry_lock);
    a->next=aliases;aliases=a;active++;allocations++;total_pieces+=a->count;
    mutexUnlock(&registry_lock);
    return a->address;
failed:
    release_storage(a);
    mutexLock(&registry_lock);held-=bytes;failures++;last_result=rc;mutexUnlock(&registry_lock);
    free(a);errno=ENOMEM;return NULL;
}
bool mineswitch_nxvk_free_cached(void* address) {
    mutexLock(&registry_lock);
    Alias** link=&aliases;
    while(*link && (*link)->address!=address)link=&(*link)->next;
    Alias* a=*link;
    if(a) {*link=a->next;active--;}
    mutexUnlock(&registry_lock);
    if(!a)return false;
    release_storage(a);
    mutexLock(&registry_lock);held-=a->bytes;mutexUnlock(&registry_lock);
    free(a);return true;
}
void mineswitch_nxvk_alias_report(void) {
    mutexLock(&registry_lock);
    size_t h=held,n=active,a=allocations,f=failures,p=total_pieces;Result rc=last_result;
    mutexUnlock(&registry_lock);
    printf("MINESWITCH_GPU_ALIAS backing=%zu active=%zu allocations=%zu failures=%zu pieces=%zu last_rc=0x%08x\n",
           h,n,a,f,p,rc);
}
