#include "mimalloc.h"
#include "page_pool.h"
#include "newlib_adapter.h"
#include "large_pages.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const size_t M=1024*1024;
static struct _reent r;
static unsigned checks;
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);abort();} checks++; } while(0)
static void* passed;
static void* passed_small;
static pthread_mutex_t hold_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_condition=PTHREAD_COND_INITIALIZER;
static unsigned hold_ready;
static bool hold_done;
static void* hold_heap(void* arg) {
    (void)arg;struct _reent local={0};void* live[16];
    for(unsigned i=0;i<16;i++) {live[i]=_malloc_r(&local,32+i*64);if(!live[i])abort();}
    pthread_mutex_lock(&hold_lock);hold_ready++;pthread_cond_broadcast(&hold_condition);
    while(!hold_done)pthread_cond_wait(&hold_condition,&hold_lock);
    pthread_mutex_unlock(&hold_lock);
    for(unsigned i=0;i<16;i++)_free_r(&local,live[i]);
    return NULL;
}
static void* producer(void* ignored) {
    (void)ignored;
    struct _reent local={0};
    passed=_malloc_r(&local,3*M);if(!passed)abort();
    memset(passed,0x67,3*M);
    passed_small=_malloc_r(&local,113);if(!passed_small)abort();
    memset(passed_small,0x58,113);
    // This heap is abandoned by the real thread destructor with a live block.
    return NULL;
}
static void* churn(void* arg) {
    size_t seed=(uintptr_t)arg;
    struct _reent local={0};
    void* blocks[128]={0};
    for(unsigned i=0;i<4000;i++) {
        size_t slot=(i*37+seed)%128,n=(i%41==0?300*1024:16)+(i*101+seed*53)%8192;
        if(blocks[slot])_free_r(&local,blocks[slot]);
        unsigned char* p=_calloc_r(&local,n,1);if(!p || p[0] || p[n-1])abort();
        p[0]=0x24;p[n-1]=0x42;blocks[slot]=p;
    }
    for(unsigned i=0;i<128;i++)_free_r(&local,blocks[i]);
    return NULL;
}
int main(void) {
    size_t capacity=(size_t)3*1024*M;
    void* region=mmap(NULL,capacity,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    CHECK(region!=MAP_FAILED);
    CHECK(nxmi_pool_init(region,capacity)==0);
    CHECK(nxmi_pool_init((char*)region+4096,capacity-4096)==EBUSY);
    void* raw=NULL;
    CHECK(nxmi_pool_alloc(4*4096,4096,false,&raw)==0);
    CHECK(nxmi_pool_zero(raw,4096)==EACCES);
    CHECK(nxmi_pool_permission(raw,4096,true)==0);
    memset(raw,0x37,4096);
    CHECK(nxmi_pool_permission((char*)raw+4096,4096,true)==0);
    CHECK(nxmi_pool_permission(raw,3*4096,false)==0);
    CHECK(nxmi_pool_free(raw,4*4096)==0); // Mixed protections normalized to RW.
    CHECK(nxmi_pool_free(raw,4*4096)==EINVAL);
    void *low1,*low2,*big,*upper_guard,*hole,*lower_guard,*mesh_fit,*big_fit;
    CHECK(nxmi_pool_alloc(M,M,true,&low1)==0);
    CHECK(nxmi_pool_alloc(16*M+16384,M,true,&big)==0);
    CHECK(nxmi_pool_alloc(2*M,M,true,&upper_guard)==0);
    CHECK(nxmi_pool_alloc(3*M,M,true,&hole)==0);
    CHECK(nxmi_pool_alloc(2*M,M,true,&lower_guard)==0);
    CHECK(nxmi_pool_alloc(M,M,true,&low2)==0);
    CHECK((uintptr_t)low2==(uintptr_t)low1+M && (uintptr_t)big>(uintptr_t)low2);
    void* scattered;size_t piece_bytes;
    CHECK(nxmi_pool_alloc_some(2*M,65536,4096,&scattered,&piece_bytes)==0);
    CHECK(piece_bytes>=65536 && piece_bytes<2*M);
    memset(scattered,0x62,piece_bytes);
    CHECK(((unsigned char*)scattered)[piece_bytes-1]==0x62);
    CHECK(nxmi_pool_free(scattered,piece_bytes)==0);
    CHECK(nxmi_pool_free(hole,3*M)==0 && nxmi_pool_free(big,16*M+16384)==0);
    CHECK(nxmi_pool_alloc(5*M/2,M,true,&mesh_fit)==0 && mesh_fit==hole);
    CHECK(nxmi_pool_alloc(16*M+16384,M,true,&big_fit)==0 && big_fit==big);
    CHECK(nxmi_pool_free(mesh_fit,5*M/2)==0 && nxmi_pool_free(big_fit,16*M+16384)==0);
    CHECK(nxmi_pool_free(upper_guard,2*M)==0 && nxmi_pool_free(lower_guard,2*M)==0);
    CHECK(nxmi_pool_free(low1,M)==0 && nxmi_pool_free(low2,M)==0);

    mi_process_init();
    unsigned char* p=_malloc_r(&r,113);CHECK(p!=NULL);memset(p,0x45,113);
    CHECK(_malloc_usable_size_r(&r,p)>=113);
    unsigned char* q=_realloc_r(&r,p,3*M);CHECK(q && q[0]==0x45 && q[112]==0x45);p=q;
    CHECK(_realloc_r(&r,p,SIZE_MAX)==NULL && r._errno==ENOMEM && p[112]==0x45);
    size_t requested,backing;
    CHECK(nxmi_large_lookup(p,&requested,&backing) && requested==3*M && backing==3*M);
    q=_realloc_r(&r,p,M+1);CHECK(q==p && q[112]==0x45);
    CHECK(nxmi_large_lookup(p,&requested,&backing) && requested==M+1 && backing==M+4096);
    q=_realloc_r(&r,p,128);CHECK(q && q[112]==0x45 && !nxmi_large_lookup(q,&requested,&backing));
    CHECK(_realloc_r(&r,q,0)==NULL);
    p=_calloc_r(&r,M,1);CHECK(p && !p[0] && !p[M-1]);
    CHECK(_reallocf_r(&r,p,SIZE_MAX)==NULL && !nxmi_large_lookup(p,&requested,&backing));
    CHECK(_calloc_r(&r,SIZE_MAX,16)==NULL);
    CHECK(_memalign_r(&r,3,4096)==NULL && r._errno==EINVAL);
    CHECK(_pvalloc_r(&r,SIZE_MAX)==NULL);
    void* untouched=(void*)(uintptr_t)0x55;
    errno=EDOM;
    CHECK(nxmi_posix_memalign(&untouched,3,4096)==EINVAL && untouched==(void*)(uintptr_t)0x55 && errno==EDOM);
    CHECK(nxmi_posix_memalign(&untouched,65536,65536)==0 && !((uintptr_t)untouched%65536) && errno==EDOM);
    _free_r(&r,untouched);
    CHECK(nxmi_aligned_alloc(4096,4097)==NULL && errno==EINVAL);
    for(size_t a=16;a<=16*M;a*=2) {
        p=_memalign_r(&r,a,5*M/2);CHECK(p && !((uintptr_t)p%a));
        p[0]=0x32;p[5*M/2-1]=0x23;
        CHECK(p[0]==0x32 && p[5*M/2-1]==0x23);_free_r(&r,p);
    }
    mi_collect(true);
    struct nxmi_pool_stats before,after;
    nxmi_pool_stats(&before,false);
    p=_memalign_r(&r,4096,16*M);CHECK(p!=NULL);
    nxmi_pool_stats(&after,false);
    CHECK(after.reserved-before.reserved<=16*M+128*1024);
    _free_r(&r,p);mi_collect(true);
    nxmi_pool_stats(&before,false);
    p=_memalign_r(&r,16*M,192*M);CHECK(p!=NULL);
    nxmi_pool_stats(&after,false);
    CHECK(after.reserved-before.reserved<=194*M);
    p[0]=0x27;p[192*M-1]=0x72;
    CHECK(p[0]==0x27 && p[192*M-1]==0x72);
    _free_r(&r,p);mi_collect(true);
    p=_memalign_r(&r,2*M,(size_t)2048*M);CHECK(p && !((uintptr_t)p%(2*M)));
    p[0]=0x76;p[(size_t)2048*M-1]=0x67;
    CHECK(p[0]==0x76 && p[(size_t)2048*M-1]==0x67);_free_r(&r,p);

    pthread_t thread;CHECK(!pthread_create(&thread,NULL,producer,NULL));CHECK(!pthread_join(thread,NULL));
    CHECK(((unsigned char*)passed)[0]==0x67 && ((unsigned char*)passed)[3*M-1]==0x67);
    _free_r(&r,passed);
    CHECK(((unsigned char*)passed_small)[112]==0x58);_free_r(&r,passed_small);
    pthread_t workers[4];
    for(uintptr_t i=0;i<4;i++)CHECK(!pthread_create(&workers[i],NULL,churn,(void*)i));
    for(unsigned i=0;i<4;i++)CHECK(!pthread_join(workers[i],NULL));

    // The real constrained-memory workload that failed with fragmented newlib:
    // a full Java heap plus many small survivors, GPU-size buffers, and meshes.
    p=_memalign_r(&r,2*M,1948*M);CHECK(p!=NULL);
    void* meta=_memalign_r(&r,16*M,192*M);CHECK(meta!=NULL);
    void* jit=_memalign_r(&r,4096,96*M);CHECK(jit!=NULL);
    pthread_t retained[32];
    for(unsigned i=0;i<32;i++)CHECK(!pthread_create(&retained[i],NULL,hold_heap,NULL));
    pthread_mutex_lock(&hold_lock);
    while(hold_ready<32)pthread_cond_wait(&hold_condition,&hold_lock);
    pthread_mutex_unlock(&hold_lock);
    void* small[160000];
    for(unsigned i=0;i<160000;i++) {
        small[i]=_malloc_r(&r,16+(i*17)%496);CHECK(small[i]!=NULL);
    }
    for(unsigned i=0;i<160000;i+=2) {_free_r(&r,small[i]);small[i]=NULL;}
    void* gpu[32];
    for(unsigned i=0;i<32;i++) {gpu[i]=_memalign_r(&r,4096,16*M);CHECK(gpu[i]!=NULL);}
    void* mesh[24];
    for(unsigned i=0;i<24;i++) {mesh[i]=_realloc_r(&r,NULL,5*M/2);CHECK(mesh[i]!=NULL);}
    nxmi_report();
    for(unsigned i=0;i<24;i++)_free_r(&r,mesh[i]);
    for(unsigned i=0;i<32;i++)_free_r(&r,gpu[i]);
    for(unsigned i=1;i<160000;i+=2)_free_r(&r,small[i]);
    pthread_mutex_lock(&hold_lock);hold_done=true;pthread_cond_broadcast(&hold_condition);pthread_mutex_unlock(&hold_lock);
    for(unsigned i=0;i<32;i++)CHECK(!pthread_join(retained[i],NULL));
    _free_r(&r,meta);_free_r(&r,jit);
    _free_r(&r,p);mi_collect(true);
    struct nxmi_pool_stats stats;nxmi_pool_stats(&stats,true);
    CHECK(stats.reserved<64*M && stats.largest_free>2048*M);
    printf("MIMALLOC_HOST_PASS checks=%u real bounded3GiB pool,2GiB alignment,mixed-permissions,cross-thread frees,fragmentation workload\n",checks);
    // Metadata still belongs to live main allocator. Process teardown owns mmap.
    return 0;
}
