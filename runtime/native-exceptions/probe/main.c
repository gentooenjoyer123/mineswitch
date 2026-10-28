#include "horizon_aarch64_context.h"
#include "nx_safefetch.h"
#include <switch.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void nx_exception_register_probe(const void* inaccessible, HorizonAArch64Context* restored);
extern char nx_exception_probe_load[], nx_exception_probe_resume[];
extern void nx_exception_trap_probe(const void* unused, HorizonAArch64Context* restored);
extern char nx_exception_probe_trap[], nx_exception_probe_trap_resume[];
static FILE* network_log;
static unsigned passed, failed;
static __thread unsigned callback_count, callback_failures;
static __thread uint32_t trap_esr, trap_description;
static atomic_bool release_workers;
static Thread workers[2];
struct WorkerResult { unsigned failures, callbacks; };
static struct WorkerResult results[2];

static void report(const char* format, ...) {
    va_list arguments;
    va_start(arguments,format); vprintf(format,arguments); va_end(arguments);
    fflush(stdout);
    if (network_log) {
        va_start(arguments,format); vfprintf(network_log,format,arguments); va_end(arguments);
        fflush(network_log);
    }
}
static bool check(const char* name,bool value) {
    if(value) ++passed; else ++failed;
    report("native.exception.check %s %s\n",name,value?"PASS":"FAIL");
    return value;
}
static bool registers_match(const HorizonAArch64Context* state) {
    if(state->x[0]!=0 || state->fpcr!=UINT64_C(0x00400000) || state->fpsr!=31) return false;
    if((state->pstate & UINT32_C(0xf0000000))!=UINT32_C(0x60000000)) return false;
    for(unsigned i=1;i<31;++i) if(state->x[i]!=i) return false;
    for(unsigned r=0;r<32;++r)
        for(unsigned lane=0;lane<16;++lane)
            if(state->vectors[r][lane]!=r+1) return false;
    return (state->sp&15)==0;
}

/* Test-only dispatcher for an exact native test instruction. The production
 * HotSpot dispatcher remains in os_horizon_aarch64.cpp; this tests its ABI. */
int horizon_aarch64_handle_exception(HorizonAArch64Context* state) {
    bool data_case=state->pc==(uintptr_t)nx_exception_probe_load && state->far==0 &&
                   (state->esr>>26)==0x24;
    bool trap_case=state->pc==(uintptr_t)nx_exception_probe_trap &&
                   *(const uint32_t*)nx_exception_probe_trap==UINT32_C(0xd4bbd5a1);
    if(!data_case && !trap_case) return 0;
    if(trap_case) { trap_esr=state->esr; trap_description=state->description; }
    ++callback_count;
    if(!registers_match(state)) ++callback_failures;
    char local;
    Thread* current=threadGetSelf();
    uintptr_t bottom=(uintptr_t)current->stack_mirror;
    if((uintptr_t)&local>=bottom && (uintptr_t)&local<bottom+current->stack_sz)
        ++callback_failures;
    u64 id=0;
    if(R_FAILED(svcGetThreadId(&id,CUR_THREAD_HANDLE)) || id==0) ++callback_failures;
    state->pc=(uintptr_t)(trap_case ? nx_exception_probe_trap_resume : nx_exception_probe_resume);
    return 1;
}
static void worker(void* argument) {
    struct WorkerResult* result=argument;
    if(horizon_aarch64_exception_thread_init()!=0) { result->failures=1; return; }
    while(!atomic_load_explicit(&release_workers,memory_order_acquire)) svcSleepThread(10000);
    HorizonAArch64Context restored;
    for(unsigned i=0;i<500;++i) {
        memset(&restored,0,sizeof(restored));
        nx_exception_register_probe(NULL,&restored);
        if(!registers_match(&restored)) ++result->failures;
    }
    result->failures+=callback_failures;
    result->callbacks=callback_count;
    horizon_aarch64_exception_thread_exit();
}
static bool probe(void) {
    check("safefetch_before_tls",nxvm_safe_fetch_word(NULL,-73)==-73);
    if(!check("main_exception_stack",horizon_aarch64_exception_thread_init()==0)) return true;
    HorizonAArch64Context restored;
    memset(&restored,0,sizeof(restored));
    nx_exception_register_probe(NULL,&restored);
    check("all_gpr_neon_flags_restored",registers_match(&restored));
    check("callback_alt_stack_and_svc",callback_count==1 && callback_failures==0);
    memset(&restored,0,sizeof(restored));
    nx_exception_trap_probe(NULL,&restored);
    check("dcps1_dead_registers_restored",registers_match(&restored));
    check("dcps1_dead_exact_callback",callback_count==2 && callback_failures==0);
    report("native.exception.trap esr=0x%08x exception_class=0x%x description=0x%08x\n",
           trap_esr,trap_esr>>26,trap_description);
    unsigned trap_class=trap_esr>>26;
    check("dcps1_dead_supported_exception_class",trap_class==0 || trap_class==0x18 || trap_class==0x3c);
    horizon_aarch64_exception_thread_exit();
    check("safefetch_after_tls",nxvm_safe_fetch32(NULL,-74)==-74);
    u64 mask=0;
    check("allowed_core_mask",R_SUCCEEDED(svcGetInfo(&mask,InfoType_CoreMask,CUR_PROCESS_HANDLE,0)));
    int cores[2]={-2,-2};
    unsigned available=0;
    for(unsigned c=0;c<32 && available<2;++c) if(mask&(UINT64_C(1)<<c)) cores[available++]=(int)c;
    if(available==1) cores[1]=cores[0];
    report("native.exception.cores=%d,%d\n",cores[0],cores[1]);
    unsigned created=0,started=0;
    for(unsigned i=0;i<2;++i) {
        if(!check("worker_create",R_SUCCEEDED(threadCreate(&workers[i],worker,&results[i],NULL,64*1024,0x2c,-2)))) break;
        ++created;
        if(cores[i]>=0 && !check("worker_core",R_SUCCEEDED(svcSetThreadCoreMask(workers[i].handle,cores[i],UINT64_C(1)<<cores[i])))) break;
        if(!check("worker_start",R_SUCCEEDED(threadStart(&workers[i])))) break;
        ++started;
    }
    atomic_store_explicit(&release_workers,true,memory_order_release);
    for(unsigned i=0;i<started;++i) {
        if(!check("worker_complete",R_SUCCEEDED(svcWaitSynchronizationSingle(workers[i].handle,INT64_C(30000000000))))) return false;
        check("worker_registers_isolated",results[i].failures==0 && results[i].callbacks==500);
    }
    for(unsigned i=0;i<created;++i)
        if(!check("worker_close",R_SUCCEEDED(threadClose(&workers[i])))) return false;
    return true;
}
int main(void) {
    consoleInit(NULL);
    Result sockets=socketInitializeDefault();
    if(R_SUCCEEDED(sockets)) {
        int fd=nxlinkConnectToHost(false,false);
        if(fd>=0) { network_log=fdopen(fd,"w"); if(!network_log) close(fd); }
    }
    report("MINESWITCH_NATIVE_EXCEPTION_BEGIN\n");
    bool complete=probe();
    report("MINESWITCH_NATIVE_EXCEPTION_END passed=%u failed=%u complete=%d\n",passed,failed,complete);
    if(!complete) { svcSleepThread(100000000); svcExitProcess(); }
    if(network_log) fclose(network_log);
    if(R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    return failed?1:0;
}
