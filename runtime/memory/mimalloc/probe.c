#include <switch.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "mimalloc.h"
#include "newlib_adapter.h"
#include "page_pool.h"
#include "../nx_memory.h"
#include "../nx_jit.h"

static const size_t M=1024*1024;
static unsigned pass,fail;
static bool check(const char* label,bool good) {
    if(good)pass++;else fail++;
    printf("MIMALLOC_CHECK %s %s errno=%d\n",label,good?"PASS":"FAIL",errno);fflush(stdout);
    return good;
}
static bool permission(void* p,u32 expected) {
    MemoryInfo info;u32 flags;
    return R_SUCCEEDED(svcQueryMemory(&info,&flags,(u64)p)) && info.perm==expected;
}
static void* transfer;
static atomic_uint thread_fail;
static UEvent hold_release;
static atomic_uint hold_ready;
static atomic_bool hold_done;
static void hold_heap(void* ignored) {
    (void)ignored;void* live[16]={0};
    for(unsigned i=0;i<16;i++) {
        live[i]=malloc(32+i*64);
        if(!live[i])atomic_fetch_add(&thread_fail,1);
    }
    atomic_fetch_add(&hold_ready,1);
    while(!atomic_load(&hold_done))
        (void)waitSingle(waiterForUEvent(&hold_release),100000000);
    for(unsigned i=0;i<16;i++)free(live[i]);
}
static void produce(void* ignored) {
    (void)ignored;
    transfer=malloc(3*M);
    if(!transfer) {atomic_fetch_add(&thread_fail,1);return;}
    memset(transfer,0x36,3*M);
}
static void churn(void* arg) {
    uintptr_t seed=(uintptr_t)arg;
    void* slots[64]={0};
    for(unsigned i=0;i<2000;i++) {
        unsigned at=(i*37+seed)%64;size_t n=32+(i*73+seed)%4096;
        free(slots[at]);slots[at]=calloc(n,1);
        if(!slots[at]) {atomic_fetch_add(&thread_fail,1);break;}
        unsigned char* p=slots[at];
        if(p[0] || p[n-1])atomic_fetch_add(&thread_fail,1);
        p[0]=0x29;p[n-1]=0x92;
    }
    for(unsigned i=0;i<64;i++)free(slots[i]);
}
static bool run_thread(Thread* t,ThreadFunc f,void* arg) {
    Result rc=threadCreate(t,f,arg,NULL,512*1024,0x2c,-2);
    if(!check("thread_create",R_SUCCEEDED(rc)))return false;
    rc=threadStart(t);
    if(!check("thread_start",R_SUCCEEDED(rc))) {threadClose(t);return false;}
    return true;
}
static void finish_thread(Thread* t) {
    check("thread_join",R_SUCCEEDED(threadWaitForExit(t)));
    check("thread_close",R_SUCCEEDED(threadClose(t)));
}
static void vm_probe(void) {
    size_t n=(size_t)2048*M;
    unsigned char* p=nxvm_reserve(n,2*M,false);
    if(!check("vm_2GiB_reserve",p!=NULL))return;
    check("vm_reserved_inaccessible",permission(p,Perm_None));
    if(check("vm_first_commit",nxvm_commit(p,4096,false)) &&
       check("vm_last_commit",nxvm_commit(p+n-4096,4096,false))) {
        p[0]=0x61;p[n-1]=0x16;
        check("vm_endpoints",p[0]==0x61 && p[n-1]==0x16);
        check("vm_uncommit",nxvm_uncommit(p,4096));
        check("vm_uncommitted_inaccessible",permission(p,Perm_None));
        if(check("vm_recommit",nxvm_commit(p,4096,false)))check("vm_recommit_zero",p[0]==0);
    }
    check("vm_release",nxvm_release(p,n));
}
static void workload(void) {
    size_t n=1948*M;
    void* heap=nxvm_reserve(n,2*M,false);
    if(!check("full_1948MiB_reserve",heap!=NULL))return;
    if(!check("full_1948MiB_commit",nxvm_commit(heap,n,false))) {nxvm_release(heap,n);return;}
    void* meta=memalign(16*M,192*M);
    NxJitArena* jit=nxjit_create(96*M,4096);
    void** small=calloc(160000,sizeof(void*));
    void* gpu[32]={0};void* mesh[24]={0};
    Thread retained[32];unsigned retained_count=0;
    ueventCreate(&hold_release,false);
    if(!check("metaspace_192MiB",meta!=NULL) || !check("jit_96MiB",jit!=NULL) ||
       !check("small_pointer_table",small!=NULL))goto cleanup;
    {
        uint32_t* rw=nxjit_rw_address(jit);
        rw[0]=0x52800540; // mov w0,#42
        rw[1]=0xd65f03c0; // ret
        nxjit_flush_instruction_cache(rw,8);
        unsigned (*code)(void)=(unsigned(*)(void))nxjit_rx_address(jit);
        check("jit_exec_real_RX",code()==42);
    }
    for(;retained_count<32;retained_count++)
        if(!run_thread(&retained[retained_count],hold_heap,NULL))break;
    while(atomic_load(&hold_ready)<retained_count)svcSleepThread(1000000);
    if(!check("retained32_thread_heaps",retained_count==32 && atomic_load(&thread_fail)==0))goto cleanup;
    unsigned got=0;
    for(;got<160000;got++) {
        small[got]=malloc(16+(got*17)%496);
        if(!small[got])break;
    }
    if(!check("small160000",got==160000))goto cleanup;
    for(unsigned i=0;i<160000;i+=2) {free(small[i]);small[i]=NULL;}
    got=0;
    for(;got<32;got++) {gpu[got]=memalign(4096,16*M);if(!gpu[got])break;}
    if(!check("graphics_sized_512MiB",got==32))goto cleanup;
    got=0;
    for(;got<24;got++) {mesh[got]=realloc(NULL,5*M/2);if(!mesh[got])break;}
    check("meshes24x2_5MiB",got==24);
cleanup:
    nxmi_report();
    atomic_store(&hold_done,true);ueventSignal(&hold_release);
    for(unsigned i=0;i<retained_count;i++)finish_thread(&retained[i]);
    for(unsigned i=0;i<24;i++)free(mesh[i]);
    for(unsigned i=0;i<32;i++)free(gpu[i]);
    if(small)for(unsigned i=0;i<160000;i++)free(small[i]);
    free(small);free(meta);
    if(jit)check("jit_close",nxjit_destroy(jit));
    check("full_heap_release",nxvm_release(heap,n));
    mi_collect(true);
}
int main(void) {
    Result rc=socketInitializeDefault();
    if(R_SUCCEEDED(rc))nxlinkStdio();
    printf("MIMALLOC_PROBE_BEGIN version=%d\n",mi_version());fflush(stdout);
    nxmi_report();
    check("active_mimalloc",mi_version()==224);
    void* p=malloc(113);check("libc_malloc",p!=NULL);
    if(p) {memset(p,0x62,113);void* q=realloc(p,3*M);
        if(check("libc_realloc",q!=NULL)){p=q;check("realloc_preserves",((unsigned char*)p)[112]==0x62);}
        check("usable_size",malloc_usable_size(p)>=113);free(p);}
    void* untouched=(void*)(uintptr_t)0x55;errno=EDOM;
    check("posix_invalid_preserves",posix_memalign(&untouched,3,4096)==EINVAL && untouched==(void*)(uintptr_t)0x55 && errno==EDOM);
    p=memalign(16*M,5*M/2);
    check("aligned16MiB",p && (uintptr_t)p%(16*M)==0);free(p);
    vm_probe();
    Thread t;
    if(run_thread(&t,produce,NULL)) {finish_thread(&t);
        check("cross_thread_live_block",transfer && ((unsigned char*)transfer)[3*M-1]==0x36);free(transfer);}
    Thread workers[4];unsigned created=0;
    for(;created<4;created++)if(!run_thread(&workers[created],churn,(void*)(uintptr_t)created))break;
    for(unsigned i=0;i<created;i++)finish_thread(&workers[i]);
    check("parallel_churn",created==4 && atomic_load(&thread_fail)==0);
    mi_collect(true);
    workload();
    nxmi_report();
    printf("MIMALLOC_PROBE_RESULT pass=%u fail=%u\n",pass,fail);fflush(stdout);
    svcSleepThread(300000000);
    AppletType type=appletGetAppletType();
    if(type==AppletType_Application || type==AppletType_SystemApplication) {
        if(!fail)appletSetTerminateResult(0);
        Service* endpoint=appletGetServiceSession_SelfController();
        if(endpoint && serviceIsActive(endpoint)) {
            Service request=*endpoint;serviceAssumeDomain(&request);
            if(R_SUCCEEDED(serviceDispatch(&request,0)))
                for(;;)svcSleepThread(1000000000);
        }
    }
    svcExitProcess();
}
