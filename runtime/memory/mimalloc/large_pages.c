#include "large_pages.h"
#include "page_pool.h"
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef MINESWITCH_MIMALLOC_HOST
#include <pthread.h>
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&registry_lock)
#define UNLOCK() pthread_mutex_unlock(&registry_lock)
#else
#include <switch.h>
static Mutex registry_lock;
#define LOCK() mutexLock(&registry_lock)
#define UNLOCK() mutexUnlock(&registry_lock)
#endif
enum { PAGE=4096, THRESHOLD=256*1024, SLOTS=8192 };
typedef struct { uintptr_t pointer;size_t requested,backing; } Allocation;
static Allocation entries[SLOTS];
static size_t used,held,peak,count;
static size_t find(uintptr_t p,bool insert) {
    size_t first=((p>>12)^(p>>25))&(SLOTS-1),tomb=SLOTS;
    for(size_t i=0;i<SLOTS;i++) {
        size_t at=(first+i)&(SLOTS-1);uintptr_t current=entries[at].pointer;
        if(current==p)return at;
        if(current==1 && tomb==SLOTS)tomb=at;
        if(!current)return insert?(tomb<SLOTS?tomb:at):SLOTS;
    }
    return insert?tomb:SLOTS;
}
bool nxmi_large_alloc(size_t bytes,size_t alignment,void** out) {
    *out=NULL;
    if(bytes<THRESHOLD)return false;
    if(bytes>PTRDIFF_MAX || bytes>SIZE_MAX-(PAGE-1))return true;
    if(alignment<PAGE)alignment=PAGE;
    size_t backing=(bytes+PAGE-1)&~(size_t)(PAGE-1);
    LOCK();bool full=used==SLOTS;UNLOCK();
    if(full)return false; // Bounded metadata; ordinary mimalloc remains available.
    void* p=NULL;
    if(nxmi_pool_alloc(backing,alignment,true,&p))return true;
    LOCK();size_t slot=find((uintptr_t)p,true);
    if(slot<SLOTS) {
        entries[slot]=(Allocation){(uintptr_t)p,bytes,backing};
        used++;held+=backing;count++;if(held>peak)peak=held;
    }
    UNLOCK();
    if(slot==SLOTS) {
        if(nxmi_pool_free(p,backing))abort();
        return false;
    }
    *out=p;return true;
}
bool nxmi_large_lookup(const void* p,size_t* requested,size_t* backing) {
    if(!p || (uintptr_t)p%PAGE)return false;
    LOCK();size_t slot=find((uintptr_t)p,false);
    if(slot<SLOTS) {*requested=entries[slot].requested;*backing=entries[slot].backing;}
    UNLOCK();return slot<SLOTS;
}
bool nxmi_large_free(void* p) {
    if(!p || (uintptr_t)p%PAGE)return false;
    LOCK();size_t slot=find((uintptr_t)p,false);size_t backing=0;
    if(slot<SLOTS) {backing=entries[slot].backing;entries[slot].pointer=1;used--;held-=backing;}
    UNLOCK();
    if(slot==SLOTS)return false;
    if(nxmi_pool_free(p,backing))abort();
    return true;
}
bool nxmi_large_shrink(void* p,size_t bytes) {
    if(bytes<THRESHOLD || bytes>SIZE_MAX-(PAGE-1))return false;
    size_t requested,backing;
    if(!nxmi_large_lookup(p,&requested,&backing) || bytes>backing)return false;
    size_t new_backing=(bytes+PAGE-1)&~(size_t)(PAGE-1);
    if(new_backing<backing && nxmi_pool_free((char*)p+new_backing,backing-new_backing))return false;
    LOCK();size_t slot=find((uintptr_t)p,false);
    if(slot==SLOTS)abort(); // Concurrent free/realloc of the same pointer is invalid.
    entries[slot].requested=bytes;entries[slot].backing=new_backing;held-=backing-new_backing;
    UNLOCK();return true;
}
void nxmi_large_report(void) {
    LOCK();size_t u=used,h=held,p=peak,c=count;UNLOCK();
    printf("MINESWITCH_LARGE_PAGES active=%zu backing=%zu peak=%zu allocations=%zu threshold=%u\n",u,h,p,c,THRESHOLD);
}
