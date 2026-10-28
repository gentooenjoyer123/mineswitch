#include "alias_platform.h"
#include "../gpu_alias.h"
#ifdef MINESWITCH_PRESSURE_HOST
#include "../cache_pressure.h"
#endif
#include "../../../memory/mimalloc/page_pool.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct VirtmemReservation { void* mapping;size_t bytes; };
static pthread_mutex_t va_lock=PTHREAD_MUTEX_INITIALIZER;
static void* pending;static size_t pending_size;
static size_t reservations,fail_at;
static _Atomic size_t mappings,map_calls;
static unsigned char* physical;
static size_t physical_size;
static int backing_fd;
static bool check_payload;
void virtmemLock(void) { pthread_mutex_lock(&va_lock); }
void virtmemUnlock(void) { pthread_mutex_unlock(&va_lock); }
void* virtmemFindStack(size_t n,size_t guard) {
    (void)guard;assert(pending==NULL);
    pending=mmap(NULL,n,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(pending==MAP_FAILED){pending=NULL;return NULL;}
    pending_size=n;return pending;
}
VirtmemReservation* virtmemAddReservation(void* address,size_t n) {
    assert(pending && (uintptr_t)address>=(uintptr_t)pending &&
           (uintptr_t)address+n<=(uintptr_t)pending+pending_size);
    VirtmemReservation* r=malloc(sizeof(*r));assert(r);
    *r=(VirtmemReservation){pending,pending_size};pending=NULL;
    reservations++;return r;
}
void virtmemRemoveReservation(VirtmemReservation* r) {
    assert(munmap(r->mapping,r->bytes)==0);free(r);reservations--;
}
Result svcMapMemory(void* destination,void* source,size_t n) {
    map_calls++;
    if(fail_at && map_calls==fail_at)return 0x991;
    uintptr_t offset=(uintptr_t)source-(uintptr_t)physical;
    assert(offset<physical_size && n<=physical_size-offset);
    void* p=mmap(destination,n,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,backing_fd,(off_t)offset);
    if(p==MAP_FAILED)return 0x992;
    assert(p==destination && mprotect(source,n,PROT_NONE)==0);
    mappings++;return 0;
}
Result svcUnmapMemory(void* destination,void* source,size_t n) {
    assert(mmap(destination,n,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==destination);
    assert(mprotect(source,n,PROT_READ|PROT_WRITE)==0);
    if(check_payload) {
        assert(((unsigned char*)source)[0]==0x9c);
        assert(((unsigned char*)source)[n-1]==0x9c);
    }
    mappings--;return 0;
}
void diagAbortWithResult(Result rc) { fprintf(stderr,"Unexpected alias abort:0x%x\n",rc);abort(); }

static pthread_barrier_t workers_ready;
static void* concurrent_aliases(void* unused) {
    (void)unused;
    for(unsigned i=0;i<8;i++) {
        size_t bytes=4*1024*1024;
        unsigned char* p=mineswitch_nxvk_alloc_cached(bytes,65536);
        assert(p && (uintptr_t)p%65536==0);
        memset(p,0x9c,bytes);
        pthread_barrier_wait(&workers_ready);
        assert(p[0]==0x9c && p[bytes-1]==0x9c);
        assert(mineswitch_nxvk_free_cached(p));
        pthread_barrier_wait(&workers_ready);
    }
    return NULL;
}
#ifdef MINESWITCH_PRESSURE_HOST
static size_t release_cached_alias(void* owner) {
    assert(mineswitch_nxvk_reclaim_caches()==0);
    assert(mineswitch_nxvk_free_cached(owner));
    mineswitch_nxvk_alias_report();
    return 16*1024*1024;
}
#endif

int main(void) {
    const size_t M=1024*1024;physical_size=64*M;
    backing_fd=memfd_create("mineswitch-gpu-alias-test",0);assert(backing_fd>=0);
    assert(ftruncate(backing_fd,physical_size)==0);
    physical=mmap(NULL,physical_size,PROT_READ|PROT_WRITE,MAP_SHARED,backing_fd,0);
    assert(physical!=MAP_FAILED && nxmi_pool_init(physical,physical_size)==0);
    void* blocks[64];
    for(unsigned i=0;i<64;i++) {
        assert(nxmi_pool_alloc(M,4096,true,&blocks[i])==0);
        memset(blocks[i],0x73,M);
    }
    for(unsigned i=0;i<64;i+=2)assert(nxmi_pool_free(blocks[i],M)==0);
    struct nxmi_pool_stats before,after;nxmi_pool_stats(&before,true);
    assert(before.largest_free==M && before.capacity-before.reserved==32*M);
    void* impossible=NULL;assert(nxmi_pool_alloc(16*M,4096,true,&impossible)==ENOMEM);
    unsigned char* a=mineswitch_nxvk_alloc_cached(16*M,4096);
    assert(a && reservations==1 && mappings>=16);
    memset(a,0x9c,16*M);check_payload=true;
    assert(mineswitch_nxvk_free_cached(a));check_payload=false;
    assert(!mineswitch_nxvk_free_cached(a) && !mineswitch_nxvk_free_cached(NULL));
    nxmi_pool_stats(&after,true);
    assert(after.reserved==before.reserved && !reservations && !mappings);
    map_calls=0;fail_at=3;
    assert(mineswitch_nxvk_alloc_cached(16*M,4096)==NULL);
    nxmi_pool_stats(&after,true);
    assert(after.reserved==before.reserved && !reservations && !mappings);
    fail_at=0;
    assert(mineswitch_nxvk_alloc_cached(64*M,4096)==NULL); // Real physical exhaustion mid-map.
    nxmi_pool_stats(&after,true);
    assert(after.reserved==before.reserved && !reservations && !mappings);
    assert(!mineswitch_nxvk_alloc_cached(M,3));
    assert(!mineswitch_nxvk_alloc_cached((size_t)1024*M+4096,4096));
    pthread_t workers[4];check_payload=true;
    assert(pthread_barrier_init(&workers_ready,NULL,4)==0);
    for(unsigned i=0;i<4;i++)assert(pthread_create(&workers[i],NULL,concurrent_aliases,NULL)==0);
    for(unsigned i=0;i<4;i++)assert(pthread_join(workers[i],NULL)==0);
    pthread_barrier_destroy(&workers_ready);check_payload=false;
    nxmi_pool_stats(&after,true);
    assert(after.reserved==before.reserved && !reservations && !mappings);
    for(unsigned i=1;i<64;i+=2) {
        assert(((unsigned char*)blocks[i])[0]==0x73 && ((unsigned char*)blocks[i])[M-1]==0x73);
        assert(nxmi_pool_free(blocks[i],M)==0);
    }
    nxmi_pool_stats(&after,true);assert(after.reserved==0 && after.largest_free==physical_size);
#ifdef MINESWITCH_PRESSURE_HOST
    void* cached=mineswitch_nxvk_alloc_cached(16*M,4096);assert(cached);
    void* survivor=NULL;
    assert(nxmi_pool_alloc(48*M,4096,true,&survivor)==0);
    memset(survivor,0x43,48*M);
    assert(mineswitch_nxvk_register_cache(cached,release_cached_alias));
    void* retried=NULL;
    assert(nxmi_pool_alloc(2637824,M,true,&retried)==ENOMEM);
    errno=EAGAIN;
    assert(mineswitch_nxvk_reclaim_caches()==16*M && errno==EAGAIN);
    mineswitch_nxvk_unregister_cache(cached);
    assert(nxmi_pool_alloc(2637824,M,true,&retried)==0);
    memset(retried,0x37,2637824);
    assert(((unsigned char*)survivor)[0]==0x43 && ((unsigned char*)survivor)[48*M-1]==0x43);
    assert(nxmi_pool_free(retried,2637824)==0 && nxmi_pool_free(survivor,48*M)==0);
    nxmi_pool_stats(&after,true);
    assert(after.reserved==0 && !reservations && !mappings);
    puts("GPU_ALIAS_PRESSURE_PASS physical exhaustion,release cached alias,real aligned retry,live guard preserved,no leaked backing");
#endif
    mineswitch_nxvk_alias_report();
    puts("GPU_ALIAS_HOST_PASS real shared backing,16MiB from1MiB holes,data roundtrip,rollback on map failure and exhaustion,4 concurrent workers,guards preserved,no leaked ranges");
    munmap(physical,physical_size);close(backing_fd);
}
