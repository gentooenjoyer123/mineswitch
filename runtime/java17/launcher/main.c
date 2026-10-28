/* Java 17 copy of runtime/launcher/main.c (probe mode). The only intended
 * differences: the runtime home is MINESWITCH_RUNTIME_HOME (default
 * /switch/mineswitch/runtime-17), banners say OpenJDK 17, and JDK 21-only
 * tuning flags are omitted. Do not add frontend code here; the integration
 * contract for the real launcher is docs/JAVA17_PORT.md. */
#ifndef MINESWITCH_RUNTIME_HOME
#define MINESWITCH_RUNTIME_HOME "/switch/mineswitch/runtime-17"
#endif
#include "install.h"
#include "config.h"
#include "clock_diagnostic.h"
#include "log_tee.h"
#ifdef MINESWITCH_GPU_ALIAS
#include "../graphics/nxvk/gpu_alias.h"
#endif
#ifdef MINESWITCH_ALLOC_TRACE
#include "alloc_trace.h"
#include "../memory/nx_memory.h"
#ifdef MINESWITCH_GRAPHICS_NXVK
#include "../graphics/nxvk/memory_trace.h"
#endif
#endif
#ifdef MINESWITCH_FRONTEND
#include "mineswitch/frontend.h"
#include "mineswitch/loading.h"
#include "mineswitch/heap_limits.h"
#include "../io/horizon_file_share.h"
#include "horizon_bridge.h"
#endif
#include <switch.h>
#include <jni.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

extern int mineswitch_register_natives(void);
extern void horizon_exit_process(int status,int report) __attribute__((noreturn));
extern uint64_t horizon_deopt_resume_count(void) __attribute__((weak));
#ifdef MINESWITCH_FRONTEND
static bool frontend_runtime_prepared;
static int frontend_log_descriptor=-1;
void mineswitch_frontend_diagnostic(const char* line) {
    if (frontend_log_descriptor<0 || !line) return;
    size_t length=strnlen(line,1024);
    send(frontend_log_descriptor,line,length,0);
    send(frontend_log_descriptor,"\n",1,0);
}
bool mineswitch_prepare_runtime(void) {
    if (!frontend_runtime_prepared) frontend_runtime_prepared=launcher_install_runtime();
    return frontend_runtime_prepared;
}
#endif
// Third-party native code can call libc exit too. Intercept before libc runs
// process-global atexit/destructor teardown against still-live VM threads.
void __wrap_exit(int status) { horizon_exit_process(status,1); }
void __wrap__exit(int status) { horizon_exit_process(status,0); }

static atomic_int stage;
static atomic_int outcome;
static atomic_bool worker_done;
static atomic_bool graphics_requested;
static atomic_bool graphics_ready;
static bool heap_precommit;
#ifdef MINESWITCH_FRONTEND
static bool loading_active;
static uint64_t java_launch_tick;
static atomic_uint_fast64_t first_game_frame_tick;
static void report_native_memory(const char* phase) {
    struct mallinfo info=mallinfo();
    printf("MINESWITCH_NATIVE_MEMORY phase=%s limit=%" PRIu64 " mapped=%" PRIu64 " available=%" PRIu64
           " free_arena=%zu top=%zu\n", phase,horizon_memory_limit(),horizon_memory_used(),
           horizon_memory_available(),info.fordblks,info.keepcost);
#ifdef MINESWITCH_GPU_ALIAS
    mineswitch_nxvk_alias_report();
#endif
    if (horizon_deopt_resume_count)
        printf("MINESWITCH_DEOPT_RESUMES count=%" PRIu64 "\n",horizon_deopt_resume_count());
#ifdef MINESWITCH_ALLOC_TRACE
    allocator_trace_snapshot();
    struct nxvm_memory_stats vm;
    nxvm_get_memory_stats(&vm);
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    size_t capacity=(uintptr_t)fake_heap_end-(uintptr_t)fake_heap_start;
    printf("MINESWITCH_VM_BACKING reservations=%zu reserved=%zu committed=%zu backing=%zu\n",
           vm.reservations,vm.reserved_bytes,vm.committed_bytes,vm.backing_bytes);
    printf("MINESWITCH_MALLOC_MEMORY capacity=%zu arena=%zu used=%zu free=%zu\n",
           capacity,info.arena,info.uordblks,info.fordblks);
#ifdef MINESWITCH_GRAPHICS_NXVK
    mineswitch_nxvk_memory_snapshot();
#endif
#endif
}
static void report_worker_cpu(Handle handle) {
    static bool unavailable;
    if (unavailable) return;
    u64 ticks=0;
    Result result=svcGetInfo(&ticks,InfoType_ThreadTickCount,handle,TickCountInfo_Total);
    if (R_SUCCEEDED(result))
        printf("MINESWITCH_MAIN_CPU ns=%" PRIu64 "\n",armTicksToNs(ticks));
    else {
        printf("MINESWITCH_MAIN_CPU unavailable_rc=0x%08x\n",result);
        unavailable=true;
    }
}
typedef struct {
    uint64_t count,total_ns,max_ns,over_50ms,over_100ms;
} FrameTiming;
static Mutex frame_timing_lock;
static FrameTiming frame_timing;
static uint64_t last_presented_tick;
// Measures intervals between completed swaps, including limiting/waits/GC.
// No Java callback, allocation, or logging occurs on the render thread.
void mineswitch_glfw_frame_presented(void) {
    uint64_t now=armGetSystemTick();
    uint_fast64_t unset=0;
    atomic_compare_exchange_strong_explicit(&first_game_frame_tick,&unset,now,
                                           memory_order_relaxed,memory_order_relaxed);
    mutexLock(&frame_timing_lock);
    if (last_presented_tick) {
        uint64_t ns=armTicksToNs(now-last_presented_tick);
        frame_timing.count++;
        frame_timing.total_ns+=ns;
        if (ns>frame_timing.max_ns) frame_timing.max_ns=ns;
        if (ns>UINT64_C(50000000)) frame_timing.over_50ms++;
        if (ns>UINT64_C(100000000)) frame_timing.over_100ms++;
    }
    last_presented_tick=now;
    mutexUnlock(&frame_timing_lock);
}
static void report_frame_timing(void) {
    mutexLock(&frame_timing_lock);
    FrameTiming value=frame_timing;
    memset(&frame_timing,0,sizeof(frame_timing));
    mutexUnlock(&frame_timing_lock);
    if (value.count && value.total_ns)
        printf("MINESWITCH_FRAME_TIME frames=%" PRIu64 " observed_ms=%.3f fps=%.2f average_ms=%.3f max_ms=%.3f over50=%" PRIu64 " over100=%" PRIu64 "\n",
            value.count,value.total_ns/1000000.0,value.count*1000000000.0/value.total_ns,
            value.total_ns/1000000.0/value.count,value.max_ns/1000000.0,value.over_50ms,value.over_100ms);
}
static void report_io_activity(void) {
    struct nxio_shared_io_stats value;
    nxio_shared_io_snapshot(&value);
    printf("MINESWITCH_IO reads=%" PRIu64 " native_reads=%" PRIu64 " native_stats=%" PRIu64
           " cache_hits=%" PRIu64 " size_hits=%" PRIu64 " logical_bytes=%" PRIu64
           " native_bytes=%" PRIu64 " cache_bytes=%" PRIu64 " cache_peak=%" PRIu64
           " native_read_ms=%.3f native_stat_ms=%.3f\n",
           value.logical_reads,value.native_reads,value.native_stats,value.cache_hits,value.size_cache_hits,
           value.logical_bytes,value.native_bytes,value.cache_bytes,value.cache_peak_bytes,
           value.native_read_ns/1000000.0,value.native_stat_ns/1000000.0);
}
// Called by GLFW on its Java/render thread before touching the native window.
void mineswitch_glfw_graphics_handoff(void) {
    if (atomic_load_explicit(&graphics_ready,memory_order_acquire)) return;
    printf("GRAPHICS_HANDOFF requested_by=glfw_window elapsed_ms=%" PRIu64 "\n",
           armTicksToNs(armGetSystemTick()-java_launch_tick)/1000000);
    atomic_store_explicit(&graphics_requested,true,memory_order_release);
    while (!atomic_load_explicit(&graphics_ready,memory_order_acquire)) svcSleepThread(1000000);
}
#endif
static bool launch_graphics;
static bool system_main;
static char launch_class[256]="HelloHorizon";
static const char* launch_classpath=".";
static const char* launch_options_file;
static const char* launch_directory;
static int launch_argc;
static char** launch_argv;
static Thread worker;
static const char* stages[]={"starting","installing runtime","creating Java VM","loading entry class",
    "running Java application","destroying Java VM","completed","failed"};

static bool java_exception(JNIEnv* env,const char* where) {
    if (!(*env)->ExceptionCheck(env)) return false;
    fprintf(stderr,"JNI_EXCEPTION phase=%s\n",where);
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return true;
}
static jlong JNICALL native_round_trip(JNIEnv* env,jclass clazz,jlong value) {
    jmethodID callback=(*env)->GetStaticMethodID(env,clazz,"nativeCallback","(J)J");
    if (!callback) return 0;
    jlong result=(*env)->CallStaticLongMethod(env,clazz,callback,value);
    if ((*env)->ExceptionCheck(env)) return 0;
    return (jlong)((uint64_t)result+UINT64_C(7));
}
static unsigned char* load_test(const char* name,size_t* size) {
    char path[512];
    if (snprintf(path,sizeof(path),"romfs:/tests/%s.class",name)>=(int)sizeof(path)) return NULL;
    struct stat st;
    if (stat(path,&st)!=0 || st.st_size<=0 || st.st_size>8*1024*1024) return NULL;
    FILE* input=fopen(path,"rb"); if (!input) return NULL;
    unsigned char* bytes=malloc((size_t)st.st_size);
    if (!bytes) { fclose(input); return NULL; }
    size_t count=fread(bytes,1,(size_t)st.st_size,input);
    bool ok=count==(size_t)st.st_size && !ferror(input);
    fclose(input);
    if (!ok) { free(bytes); return NULL; }
    *size=count; return bytes;
}
#ifdef MINESWITCH_FRONTEND
static jobject start_progress_sampler(JNIEnv* env) {
    const char* name="mineswitch/launcher/ProgressSampler";
    size_t size=0;
    unsigned char* bytes=load_test(name,&size);
    if (!bytes) { printf("MINESWITCH_SAMPLER_UNAVAILABLE reason=class_resource\n"); return NULL; }
    jclass loaders=(*env)->FindClass(env,"java/lang/ClassLoader");
    jmethodID system=loaders?(*env)->GetStaticMethodID(env,loaders,"getSystemClassLoader","()Ljava/lang/ClassLoader;"):NULL;
    jobject loader=system?(*env)->CallStaticObjectMethod(env,loaders,system):NULL;
    jclass sampler=NULL,threads=NULL;
    jobject thread=NULL,result=NULL;
    if (!java_exception(env,"sampler loader") && loader) {
        sampler=(*env)->DefineClass(env,name,loader,(const jbyte*)bytes,(jsize)size);
        if (!java_exception(env,"sampler class") && sampler) {
            threads=(*env)->FindClass(env,"java/lang/Thread");
            jmethodID current=threads?(*env)->GetStaticMethodID(env,threads,"currentThread","()Ljava/lang/Thread;"):NULL;
            thread=current?(*env)->CallStaticObjectMethod(env,threads,current):NULL;
            if (!java_exception(env,"sampler thread") && thread) {
                jmethodID start=(*env)->GetStaticMethodID(env,sampler,"startIfEnabled","(Ljava/lang/Thread;)Lmineswitch/launcher/ProgressSampler;");
                if (!java_exception(env,"sampler method") && start)
                    result=(*env)->CallStaticObjectMethod(env,sampler,start,thread);
                if (java_exception(env,"sampler start")) result=NULL;
            }
        }
    }
    if (thread) (*env)->DeleteLocalRef(env,thread);
    if (threads) (*env)->DeleteLocalRef(env,threads);
    if (sampler) (*env)->DeleteLocalRef(env,sampler);
    if (loader) (*env)->DeleteLocalRef(env,loader);
    if (loaders) (*env)->DeleteLocalRef(env,loaders);
    free(bytes);
    return result;
}
static void stop_progress_sampler(JNIEnv* env,jobject sampler) {
    if (!sampler) return;
    jclass type=(*env)->GetObjectClass(env,sampler);
    jmethodID close=type?(*env)->GetMethodID(env,type,"close","()V"):NULL;
    if (!java_exception(env,"sampler close method") && close) (*env)->CallVoidMethod(env,sampler,close);
    java_exception(env,"sampler close");
    if (type) (*env)->DeleteLocalRef(env,type);
    (*env)->DeleteLocalRef(env,sampler);
}
#endif
static int run_java(void) {
#ifdef MINESWITCH_ALLOC_TRACE
    allocator_trace_start();
#endif
    atomic_store(&stage,1);
#ifdef MINESWITCH_FRONTEND
    if (!mineswitch_prepare_runtime()) return 10;
#else
    if (!launcher_install_runtime()) return 10;
#endif
    if (chdir("sdmc:/")!=0) { fprintf(stderr,"SD chdir: %s\n",strerror(errno)); return 11; }
    if (mkdir("/switch/mineswitch/tmp",0777)!=0 && errno!=EEXIST) return 12;
    if (chdir("/switch/mineswitch")!=0) return 15;
    if (launch_directory && chdir(launch_directory)!=0) {
        fprintf(stderr,"INSTANCE_DIRECTORY failed: %s\n",strerror(errno)); return 15;
    }
    if (setenv("MINESWITCH_JAVA_HOME",MINESWITCH_RUNTIME_HOME,1)!=0) return 13;
    int registry=mineswitch_register_natives();
    printf("NATIVE_REGISTRY result=%d\n",registry);
    if (registry!=0) return 14;
    size_t cp_size=strlen(launch_classpath)+sizeof("-Djava.class.path=");
    char* cp_option=malloc(cp_size);
    if (!cp_option) return 16;
    snprintf(cp_option,cp_size,"-Djava.class.path=%s",launch_classpath);
    JavaVMOption options[]={
        {.optionString=cp_option},
        {.optionString="-Dorg.lwjgl.system.allocator=system"},
        {.optionString="-Xms16m"},
        {.optionString="-Xmx64m"},
        {.optionString="-Xss1536k"},
#if MINESWITCH_ZERO_VM
        {.optionString="-Xint"},
#endif
        {.optionString="-Xrs"},
        {.optionString="-XX:+UseSerialGC"},
        {.optionString="-XX:-UsePerfData"},
        {.optionString="-XX:NativeMemoryTracking=off"},
        {.optionString="-XX:-UseCompressedOops"},
        {.optionString="-XX:-UseCompressedClassPointers"},
        {.optionString="-Djava.home=" MINESWITCH_RUNTIME_HOME},
        {.optionString="-Djava.io.tmpdir=/switch/mineswitch/tmp"},
        {.optionString="-Duser.home=/switch/mineswitch"},
        {.optionString="-Duser.dir=/switch/mineswitch"},
        {.optionString="-Dfile.encoding=UTF-8"},
        {.optionString="-Djdk.image.map.all=false"},
        {.optionString="--add-modules=ALL-SYSTEM"},
        {.optionString="-XX:ErrorFile=/switch/mineswitch/hs_err_pid%p.log"},
        {.optionString="-Xlog:gc=info,os+thread=info"},
        // CodeMemory pins the whole reservation eagerly (docs/HORIZON_JIT_MEMORY.md);
        // JDK 17's tiered default (240 MiB) is not a sensible probe budget.
        // Options files are appended later and may override this value.
        {.optionString="-XX:ReservedCodeCacheSize=64m"}
    };
    LauncherOptions extra={0};
    if (launch_options_file && !launcher_read_options(launch_options_file,&extra)) { free(cp_option); return 17; }
    size_t defaults=sizeof(options)/sizeof(options[0]);
    JavaVMOption* combined=malloc((defaults+extra.count+3)*sizeof(*combined));
    if (!combined) { free(cp_option); launcher_free_options(&extra); return 18; }
    memcpy(combined,options,sizeof(options));
    if (extra.count) memcpy(combined+defaults,extra.values,extra.count*sizeof(*combined));
    size_t option_count=defaults+extra.count;
#ifdef MINESWITCH_COMPACT_VM
    combined[option_count++]=(JavaVMOption){.optionString="-XX:ReservedCodeCacheSize=64m"};
    // JDK 17 still has the NMethodSweeper; SweeperThreshold is a JDK 17 flag too
    // (percent of ReservedCodeCacheSize), so the same value is kept.
    combined[option_count++]=(JavaVMOption){.optionString="-XX:SweeperThreshold=40"};
    printf("MINESWITCH_COMPACT_VM code_cache_mib=64 sweeper_threshold_percent=40\n");
#endif
    char initial_heap[64];
    if (heap_precommit) {
        const char* maximum=NULL;
        for (size_t i=0;i<option_count;i++)
            if (!strncmp(combined[i].optionString,"-Xmx",4)) maximum=combined[i].optionString;
        if (!maximum || strlen(maximum)>=sizeof(initial_heap)) {
            free(combined); launcher_free_options(&extra); free(cp_option); return 19;
        }
        snprintf(initial_heap,sizeof(initial_heap),"-Xms%s",maximum+4);
        combined[option_count++]=(JavaVMOption){.optionString=initial_heap};
        printf("MINESWITCH_HEAP_PRECOMMIT option=%s\n",initial_heap);
    }
    JavaVMInitArgs arguments={.version=JNI_VERSION_1_8,
        .nOptions=(jint)option_count,.options=combined,.ignoreUnrecognized=JNI_FALSE};
    const char* effective_initial="unset";const char* effective_maximum="unset";
    for(size_t i=0;i<option_count;i++) {
        if(!strncmp(combined[i].optionString,"-Xms",4))effective_initial=combined[i].optionString;
        if(!strncmp(combined[i].optionString,"-Xmx",4))effective_maximum=combined[i].optionString;
    }
    printf("MINESWITCH_EFFECTIVE_HEAP initial=%s maximum=%s precommit=%d\n",
           effective_initial,effective_maximum,heap_precommit);
    JavaVM* vm=NULL; JNIEnv* env=NULL;
    launcher_clock_diagnostic();
    atomic_store(&stage,2);
    printf("JNI_CREATE_BEGIN stack_bytes=%zu default_heap_max=67108864\n",threadGetSelf()->stack_sz);
#ifdef MINESWITCH_FRONTEND
    report_native_memory("before_vm");
#endif
    jint result=JNI_CreateJavaVM(&vm,(void**)&env,&arguments);
    free(combined); launcher_free_options(&extra); free(cp_option);
    printf("JNI_CREATE_END result=%d vm=%p env=%p\n",result,(void*)vm,(void*)env);
#ifdef MINESWITCH_FRONTEND
    report_native_memory("after_vm");
#endif
    if (result!=JNI_OK || !vm || !env) return 20;
    printf("JNI_INTERFACE_VERSION value=0x%x\n",(*env)->GetVersion(env));
#ifdef MINESWITCH_FRONTEND
    jobject progress_sampler=launch_graphics?start_progress_sampler(env):NULL;
#endif
    int status=0;
#ifndef MINESWITCH_FRONTEND
    if (launch_graphics) {
        printf("GRAPHICS_HANDOFF requested\n");
        atomic_store_explicit(&graphics_requested,true,memory_order_release);
        while (!atomic_load_explicit(&graphics_ready,memory_order_acquire)) svcSleepThread(1000000);
    }
#endif
    atomic_store(&stage,3);
    size_t class_size=0;
    jclass test=NULL;
    if (system_main) {
        test=(*env)->FindClass(env,launch_class);
        if (java_exception(env,"FindClass main") || !test) status=22;
    } else {
        unsigned char* bytes=load_test(launch_class,&class_size);
        if (!bytes) { fprintf(stderr,"TEST_CLASS read failed: %s\n",strerror(errno)); status=21; }
        else {
            jclass loaders=(*env)->FindClass(env,"java/lang/ClassLoader");
            jmethodID system=loaders?(*env)->GetStaticMethodID(env,loaders,"getSystemClassLoader","()Ljava/lang/ClassLoader;"):NULL;
            jobject loader=system?(*env)->CallStaticObjectMethod(env,loaders,system):NULL;
            if (java_exception(env,"system class loader") || !loader) status=22;
            else {
                test=(*env)->DefineClass(env,launch_class,loader,(const jbyte*)bytes,(jsize)class_size);
                if (java_exception(env,"DefineClass") || !test) status=22;
                (*env)->DeleteLocalRef(env,loader);
            }
            if (loaders) (*env)->DeleteLocalRef(env,loaders);
            free(bytes);
        }
    }
    if (!status && !strcmp(launch_class,"HelloHorizon")) {
        JNINativeMethod methods[]={{.name="nativeRoundTrip",.signature="(J)J",.fnPtr=(void*)native_round_trip}};
        jint registration=(*env)->RegisterNatives(env,test,methods,1);
        printf("JNI_REGISTER_NATIVES result=%d\n",registration);
        if (java_exception(env,"RegisterNatives") || registration!=JNI_OK) status=23;
    }
    if (!status) {
        jmethodID main_method=(*env)->GetStaticMethodID(env,test,"main","([Ljava/lang/String;)V");
        jclass string=(*env)->FindClass(env,"java/lang/String");
        if (java_exception(env,"main lookup") || !main_method || !string) status=24;
        else {
            jobjectArray args=(*env)->NewObjectArray(env,launch_argc,string,NULL);
            for (int i=0;args && i<launch_argc && !(*env)->ExceptionCheck(env);i++) {
                jstring value=(*env)->NewStringUTF(env,launch_argv[i]);
                if (!value) break;
                (*env)->SetObjectArrayElement(env,args,i,value);
                (*env)->DeleteLocalRef(env,value);
            }
            if (java_exception(env,"main arguments") || !args) status=25;
            else {
                atomic_store(&stage,4);
                printf("JAVA_MAIN_BEGIN class=%s class_bytes=%zu\n",launch_class,class_size);
                (*env)->CallStaticVoidMethod(env,test,main_method,args);
                if (java_exception(env,launch_class)) status=26;
                printf("JAVA_MAIN_END result=%d\n",status);
                (*env)->DeleteLocalRef(env,args);
            }
            (*env)->DeleteLocalRef(env,string);
        }
    }
#ifdef MINESWITCH_FRONTEND
    stop_progress_sampler(env,progress_sampler);
#endif
    if (test) (*env)->DeleteLocalRef(env,test);
    atomic_store(&stage,5);
    printf("JNI_DESTROY_BEGIN\n");
    jint destroyed=(*vm)->DestroyJavaVM(vm);
    printf("JNI_DESTROY_END result=%d\n",destroyed);
    if (destroyed!=JNI_OK && !status) status=27;
    return status;
}
static void java_worker(void* unused) {
    (void)unused;
    int status=run_java();
    atomic_store(&outcome,status);
    atomic_store(&stage,status?7:6);
    printf("MINESWITCH_JVM_WORKER_END result=%d\n",status);
    if (horizon_deopt_resume_count)
        printf("MINESWITCH_DEOPT_RESUMES count=%" PRIu64 "\n",horizon_deopt_resume_count());
    atomic_store_explicit(&worker_done,true,memory_order_release);
}
int main(int argc,char** argv) {
#ifdef MINESWITCH_GRAPHICS_NXVK
    // Select the statically linked GL-over-Vulkan driver before SDL/GLFW starts.
    // Maxwell uses the upstream NVK explicit opt-in for non-conformant hardware.
    if (setenv("MESA_LOADER_DRIVER_OVERRIDE","zink",1) ||
        setenv("GALLIUM_DRIVER","zink",1) ||
        setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER","1",1)) return 2;
#endif
    for (int i=1;i<argc && strcmp(argv[i],"--");i++)
        if (!strcmp(argv[i],"--precommit-heap")) heap_precommit=true;
#ifdef MINESWITCH_FRONTEND
    int heap_override_mib=0;
    for (int i=1;i<argc;i++) if (!strncmp(argv[i],"--heap-mib=",11)) {
        char* end=NULL;
        errno=0;
        unsigned long value=strtoul(argv[i]+11,&end,10);
        if (errno || end==argv[i]+11 || *end || value<256 || value>MINESWITCH_PORTABLE_MAX_HEAP_MIB) {
            fprintf(stderr,"Invalid --heap-mib: expected256..%d; device limit checked by launcher\n",MINESWITCH_PORTABLE_MAX_HEAP_MIB);
            return 2;
        }
        heap_override_mib=(int)value;
    }
    Result sockets=socketInitializeDefault();
    int descriptor=R_SUCCEEDED(sockets)?nxlinkConnectToHost(false,false):-1;
    frontend_log_descriptor=descriptor;
    if (descriptor>=0) {
        struct timeval timeout={.tv_sec=0,.tv_usec=100000};
        setsockopt(descriptor,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        const char* text="MINESWITCH_FRONTEND_BEGIN version=0.2.1\n";
        send(descriptor,text,strlen(text),0);
        Result clock_service=timeInitialize();
        if (R_SUCCEEDED(clock_service)) {
            const TimeType types[]={TimeType_UserSystemClock,TimeType_LocalSystemClock,TimeType_NetworkSystemClock};
            const char* names[]={"user","local","network"};
            for (unsigned i=0;i<3;i++) {
                u64 seconds=0;
                Result clock_result=timeGetCurrentTime(types[i],&seconds);
                char line[144];
                int length=snprintf(line,sizeof(line),"MINESWITCH_FRONTEND_CLOCK name=%s rc=0x%08x posix=%" PRIu64 "\n",names[i],clock_result,seconds);
                if (length>0 && length<(int)sizeof(line)) send(descriptor,line,(size_t)length,0);
            }
            timeExit();
        }
    }
    Result mounted=romfsInit();
    if (R_FAILED(mounted)) return 1;
    MineSwitchLaunch selected={0};
    int frontend=mineswitch_frontend("sdmc:/switch/mineswitch",&selected,heap_override_mib);
    if (descriptor>=0) {
        char message[160];
        int size=snprintf(message,sizeof(message),"MINESWITCH_FRONTEND_END result=%d preparation=%d\n",frontend,selected.preparation_only);
        if (size>0) send(descriptor,message,(size_t)size,0);
        mineswitch_frontend_diagnostic(selected.preparation_summary);
        if (frontend<0) {
            const char* reason=mineswitch_frontend_error();
            send(descriptor,reason,strlen(reason),0); send(descriptor,"\n",1,0);
        }
    }
    if (frontend<=0) {
        if (frontend<0) {
            consoleInit(NULL);
            printf("MineSwitch launcher could not start:\n%s\n\nPress + to exit.\n",mineswitch_frontend_error());
            padConfigureInput(1,HidNpadStyleSet_NpadStandard);
            PadState error_pad; padInitializeDefault(&error_pad);
            while (appletMainLoop()) {
                padUpdate(&error_pad);
                if (padGetButtonsDown(&error_pad)&HidNpadButton_Plus) break;
                consoleUpdate(NULL); svcSleepThread(16000000);
            }
        }
        mineswitch_free_launch(&selected);
        horizon_exit_process(frontend<0?1:0,1);
    }
    size_t selected_size=strlen(selected.main_class);
    if (!selected_size || selected_size>=sizeof(launch_class)) horizon_exit_process(2,1);
    for (size_t i=0;i<=selected_size;i++) launch_class[i]=selected.main_class[i]=='.'?'/':selected.main_class[i];
    launch_options_file=selected.options_file;
    launch_directory=selected.working_directory;
    launch_argc=selected.argc;
    launch_argv=selected.argv;
    launch_graphics=!selected.preparation_only;
    system_main=true;
#endif
#if MINESWITCH_DEFAULT_GAME
    bool custom_entry=false;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--")) break;
        if (!strncmp(argv[i],"--class=",8)) custom_entry=true;
    }
    if (!custom_entry) {
        launch_graphics=true;
        system_main=true;
        strcpy(launch_class,"mineswitch/launcher/ClientEntry");
        launch_options_file="romfs:/launch/minecraft.options";
    }
#endif
#ifndef MINESWITCH_FRONTEND
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--graphics")) launch_graphics=true;
        else if (!strcmp(argv[i],"--system-main")) system_main=true;
        else if (!strncmp(argv[i],"--jvm-options=",14)) launch_options_file=argv[i]+14;
        else if (!strncmp(argv[i],"--classpath=",12)) launch_classpath=argv[i]+12;
        else if (!strncmp(argv[i],"--class=",8)) {
            const char* name=argv[i]+8;
            size_t n=strlen(name);
            if (!n || n>=sizeof(launch_class) || strstr(name,"..")) return 2;
            for (size_t j=0;j<n;j++) {
                char c=name[j];
                if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
                      (c>='0'&&c<='9') || c=='_' || c=='$' || c=='.')) return 2;
                launch_class[j]=c=='.'?'/':c;
            }
            launch_class[n]=0;
        } else if (!strcmp(argv[i],"--")) { launch_argc=argc-i-1; launch_argv=argv+i+1; break; }
    }
#endif
    consoleInit(NULL);
#ifndef MINESWITCH_FRONTEND
    Result sockets=socketInitializeDefault();
    int descriptor=R_SUCCEEDED(sockets)?nxlinkConnectToHost(false,false):-1;
#endif
    launcher_log_init(descriptor);
    printf("MINESWITCH_JVM_BEGIN version=0.1.0-java17 applet=%d firmware=%u.%u.%u nxlink=%d\n",
        (int)appletGetAppletType(),HOSVER_MAJOR(hosversionGet()),HOSVER_MINOR(hosversionGet()),
        HOSVER_MICRO(hosversionGet()),descriptor>=0);
    printf("Real OpenJDK 17 %s bootstrap. Plus exits before graphics handoff.\n",
        MINESWITCH_ZERO_VM ? "Zero" : "AArch64 HotSpot");
#ifndef MINESWITCH_FRONTEND
    Result mounted=romfsInit();
#endif
    if (R_FAILED(mounted)) {
        fprintf(stderr,"ROMFS_INIT failed=0x%08x\n",mounted);
        launcher_log_drain(); horizon_exit_process(1,1);
    }
#ifdef MINESWITCH_FRONTEND
    if (launch_graphics) {
        launcher_console_release();
        loading_active=mineswitch_loading_begin("Minecraft Java Edition")!=0;
        if (!loading_active) {
            launcher_console_restore();
            printf("Starting Minecraft. Graphical loading screen is unavailable.\n");
        }
        printf("LOADING_SCREEN active=%d\n",loading_active);
    }
#endif
#ifdef MINESWITCH_FRONTEND
    java_launch_tick=armGetSystemTick();
    bool first_game_frame_reported=false;
#endif
    Result created=threadCreate(&worker,java_worker,NULL,NULL,4u*1024u*1024u,0x3B,-2);
    if (R_SUCCEEDED(created)) created=threadStart(&worker);
    if (R_FAILED(created)) {
        fprintf(stderr,"JVM_WORKER_CREATE failed=0x%08x\n",created);
#ifdef MINESWITCH_FRONTEND
        if (loading_active) { mineswitch_loading_end(); loading_active=false; }
#endif
        launcher_log_drain(); horizon_exit_process(2,1);
    }
    padConfigureInput(1,HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    uint64_t started=armGetSystemTick(),last_heartbeat=started;
    bool aborted=false;
    while (appletMainLoop()) {
        if (launcher_native_fault_pending()) {
            launcher_log_pump();
            svcSleepThread(1000000);
            continue;
        }
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad)&HidNpadButton_Plus) &&
            !atomic_load_explicit(&graphics_ready,memory_order_relaxed)) { aborted=true; break; }
        if (atomic_load_explicit(&graphics_requested,memory_order_acquire) &&
            !atomic_load_explicit(&graphics_ready,memory_order_relaxed)) {
#ifdef MINESWITCH_FRONTEND
            if (loading_active) { mineswitch_loading_end(); loading_active=false; }
#endif
            launcher_console_release();
            atomic_store_explicit(&graphics_ready,true,memory_order_release);
            printf("GRAPHICS_HANDOFF complete\n");
        }
        launcher_log_pump();
        if (launcher_native_fault_pending()) continue;
#ifdef MINESWITCH_FRONTEND
        uint64_t first_frame=atomic_load_explicit(&first_game_frame_tick,memory_order_relaxed);
        if(first_frame && !first_game_frame_reported) {
            first_game_frame_reported=true;
            printf("STARTUP_FIRST_GAME_FRAME elapsed_ms=%" PRIu64 "\n",
                   armTicksToNs(first_frame-java_launch_tick)/1000000);
        }
        if (loading_active) mineswitch_loading_frame(atomic_load(&stage));
        else launcher_console_update();
#else
        launcher_console_update();
#endif
        // The supervisor has higher priority than the JVM worker. Yield even
        // before graphics handoff: try-lock log pumping must not busy-spin and
        // starve the lower-priority thread that is producing the log output.
        svcSleepThread(16000000);
        uint64_t now=armGetSystemTick();
        if (armTicksToNs(now-last_heartbeat)>=UINT64_C(5000000000)) {
            int current=atomic_load(&stage);
            printf("JVM_HEARTBEAT elapsed_s=%" PRIu64 " stage=%s\n",
                armTicksToNs(now-started)/UINT64_C(1000000000),stages[current]);
#ifdef MINESWITCH_FRONTEND
            report_frame_timing();
            report_io_activity();
            report_native_memory("running");
            report_worker_cpu(worker.handle);
#endif
            last_heartbeat=now;
        }
        if (atomic_load_explicit(&worker_done,memory_order_acquire)) break;
    }
#ifdef MINESWITCH_FRONTEND
    if (loading_active) { mineswitch_loading_end(); loading_active=false; }
#endif
    if (atomic_load_explicit(&worker_done,memory_order_acquire)) {
        int32_t index=-1; Handle handle=worker.handle;
        Result exited=svcWaitSynchronization(&index,&handle,1,INT64_C(5000000000));
        printf("JVM_WORKER_NATIVE_EXIT result=0x%08x\n",exited);
        if (R_SUCCEEDED(exited)) threadClose(&worker);
        printf("MINESWITCH_JVM_END result=%d exit=process\n",atomic_load(&outcome));
    } else {
        printf("MINESWITCH_JVM_END aborted=%d stage=%s exit=process\n",aborted,stages[atomic_load(&stage)]);
    }
    launcher_log_drain();
    // Never return into hbloader with HotSpot daemon/reaper threads still alive.
    horizon_exit_process(atomic_load_explicit(&worker_done,memory_order_acquire)
                         ? atomic_load(&outcome) : 0,1);
}
