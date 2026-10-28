#include "log_tee.h"
#include <switch.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define QUEUE_SIZE (1024u*1024u)
static unsigned char queue[QUEUE_SIZE];
static size_t head,used,dropped;
static Mutex output_mutex;
static bool console_enabled=true;
static const devoptab_t *saved_stdout,*saved_stderr;
static devoptab_t tee_stdout,tee_stderr;
static int network=-1;
static unsigned char pending[16384];
static size_t pending_at,pending_size;

/* A separate first-fault slot avoids taking output_mutex from a faulting VM
 * thread, which might already own that lock. Only the main log pump formats
 * or sends the record. 0=empty, 1=publishing, 2=ready, 3=reported. */
static atomic_uint native_fault_state;
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"fault publication requires lock-free atomics");
static struct {
    uint64_t pc,far,sp,lr,fp,thread_id;
    uint32_t esr,description;
    uint64_t registers[31],rw_pc;
    uint32_t instructions[12],rw_instructions[12],instruction_mask,rw_mask;
    int java_state;
    char stop_message[160];
} native_fault;
static char native_fault_line[2048];
static size_t native_fault_length,native_fault_sent;
static bool native_fault_console_done;

bool launcher_native_fault_pending(void) {
    return atomic_load_explicit(&native_fault_state,memory_order_acquire)!=0;
}

static char* append_literal(char* out,const char* value) {
    while (*value) *out++=*value++;
    return out;
}
static char* append_hex(char* out,uint64_t value,unsigned digits) {
    static const char alphabet[]="0123456789abcdef";
    for (unsigned remaining=digits;remaining;--remaining)
        *out++=alphabet[(value>>((remaining-1)*4))&15];
    return out;
}
extern void* nxjit_writable_address(const void*) __attribute__((weak));
/* A bounded diagnostic of mapped readable code only; no allocator, VM walk or
 * output lock on the faulting thread. Unknown instructions stay unhandled. */
static uint32_t snapshot_instructions(uint64_t pc,uint32_t words[12]) {
    if (pc<16 || pc>UINT64_MAX-32 || (pc&3)) return 0;
    uint32_t mask=0;
    for (unsigned i=0;i<12;i++) {
        uint64_t address=pc-16+4*i;
        MemoryInfo info; u32 page_info;
        if (R_SUCCEEDED(svcQueryMemory(&info,&page_info,address)) &&
                (info.perm&Perm_R) && info.size>=4 && address-info.addr<=info.size-4) {
            words[i]=*(const volatile uint32_t*)(uintptr_t)address;
            mask|=1u<<i;
        }
    }
    return mask;
}
void horizon_native_fault_report(uint64_t pc,uint64_t far,uint64_t sp,
        uint64_t lr,uint64_t fp,uint64_t thread_id,uint32_t esr,uint32_t description,
        const uint64_t* registers,int java_state) {
    unsigned empty=0;
    if (atomic_compare_exchange_strong_explicit(&native_fault_state,&empty,1,
            memory_order_acq_rel,memory_order_acquire)) {
        native_fault.pc=pc; native_fault.far=far; native_fault.sp=sp;
        native_fault.lr=lr; native_fault.fp=fp; native_fault.thread_id=thread_id;
        native_fault.esr=esr; native_fault.description=description;
        native_fault.java_state=java_state;
        for (unsigned i=0;i<31;i++) native_fault.registers[i]=registers[i];
        native_fault.rw_pc=nxjit_writable_address?(uintptr_t)nxjit_writable_address((void*)(uintptr_t)pc):pc;
        native_fault.instruction_mask=snapshot_instructions(pc,native_fault.instructions);
        native_fault.rw_mask=snapshot_instructions(native_fault.rw_pc,native_fault.rw_instructions);
        if ((native_fault.instruction_mask&0x70)==0x70 && native_fault.instructions[4]==0xd4bbd5c1u) {
            uint64_t message=(uint64_t)native_fault.instructions[5]|((uint64_t)native_fault.instructions[6]<<32);
            MemoryInfo info; u32 page_info;
            if (R_SUCCEEDED(svcQueryMemory(&info,&page_info,message)) && (info.perm&Perm_R) &&
                    info.size>=sizeof(native_fault.stop_message) && message-info.addr<=info.size-sizeof(native_fault.stop_message)) {
                for (unsigned i=0;i<sizeof(native_fault.stop_message)-1;i++) {
                    unsigned char c=*(const volatile unsigned char*)(uintptr_t)(message+i);
                    if (!c) break;
                    native_fault.stop_message[i]=(c>=32 && c<127)?(char)c:'?';
                }
            }
        }
        atomic_store_explicit(&native_fault_state,2,memory_order_release);
    }
    uint64_t start=armGetSystemTick();
    while (atomic_load_explicit(&native_fault_state,memory_order_acquire)!=3 &&
           armTicksToNs(armGetSystemTick()-start)<UINT64_C(500000000))
        svcSleepThread(1000000);
}
static void pump_native_fault(void) {
    if (atomic_load_explicit(&native_fault_state,memory_order_acquire)!=2) return;
    if (!native_fault_length) {
        char* p=native_fault_line;
        p=append_literal(p,"\nHORIZON_NATIVE_UNHANDLED pc=0x"); p=append_hex(p,native_fault.pc,16);
        p=append_literal(p," far=0x"); p=append_hex(p,native_fault.far,16);
        p=append_literal(p," sp=0x"); p=append_hex(p,native_fault.sp,16);
        p=append_literal(p," lr=0x"); p=append_hex(p,native_fault.lr,16);
        p=append_literal(p," fp=0x"); p=append_hex(p,native_fault.fp,16);
        p=append_literal(p," tid=0x"); p=append_hex(p,native_fault.thread_id,16);
        p=append_literal(p," esr=0x"); p=append_hex(p,native_fault.esr,8);
        p=append_literal(p," ec=0x"); p=append_hex(p,native_fault.esr>>26,2);
        p=append_literal(p," description=0x"); p=append_hex(p,native_fault.description,8);
        p=append_literal(p," reporter=0x"); p=append_hex(p,(uintptr_t)horizon_native_fault_report,16);
        *p++='\n';
        p=append_literal(p,"HORIZON_FAULT_CODE java_state=0x"); p=append_hex(p,(uint32_t)native_fault.java_state,8);
        p=append_literal(p," rw_pc=0x"); p=append_hex(p,native_fault.rw_pc,16);
        p=append_literal(p," rx_mask=0x"); p=append_hex(p,native_fault.instruction_mask,8);
        p=append_literal(p," rw_mask=0x"); p=append_hex(p,native_fault.rw_mask,8);
        p=append_literal(p," start=pc-16 rx=");
        for (unsigned i=0;i<12;i++) { p=append_hex(p,native_fault.instructions[i],8); *p++=' '; }
        p=append_literal(p,"rw=");
        for (unsigned i=0;i<12;i++) { p=append_hex(p,native_fault.rw_instructions[i],8); *p++=' '; }
        *p++='\n';
        p=append_literal(p,"HORIZON_FAULT_REGISTERS x0..x30=");
        for (unsigned i=0;i<31;i++) { p=append_hex(p,native_fault.registers[i],16); *p++=' '; }
        *p++='\n';
        if (native_fault.stop_message[0]) {
            p=append_literal(p,"HORIZON_FAULT_STOP "); p=append_literal(p,native_fault.stop_message); *p++='\n';
        }
        native_fault_length=(size_t)(p-native_fault_line);
    }
    if (network>=0 && native_fault_sent<native_fault_length) {
        ssize_t sent=send(network,native_fault_line+native_fault_sent,
                         native_fault_length-native_fault_sent,MSG_DONTWAIT);
        if (sent>0) native_fault_sent+=(size_t)sent;
    }
    if (!native_fault_console_done && mutexTryLock(&output_mutex)) {
        if (console_enabled && saved_stdout && saved_stdout->write_r)
            saved_stdout->write_r(_REENT,NULL,native_fault_line,native_fault_length);
        native_fault_console_done=true;
        mutexUnlock(&output_mutex);
    }
    if ((network>=0 && native_fault_sent==native_fault_length) ||
            (network<0 && native_fault_console_done))
        atomic_store_explicit(&native_fault_state,3,memory_order_release);
}

static ssize_t write_tee(struct _reent* reent,void* fd,const char* bytes,size_t size,bool error) {
    mutexLock(&output_mutex);
    (void)error;
    const devoptab_t* original=saved_stdout;
    if (console_enabled && original && original->write_r) original->write_r(reent,fd,bytes,size);
    if (network>=0) {
        size_t count=size<QUEUE_SIZE-used?size:QUEUE_SIZE-used;
        size_t tail=(head+used)%QUEUE_SIZE;
        size_t first=count<QUEUE_SIZE-tail?count:QUEUE_SIZE-tail;
        memcpy(queue+tail,bytes,first);
        memcpy(queue,bytes+first,count-first);
        used+=count; dropped+=size-count;
    }
    mutexUnlock(&output_mutex);
    return (ssize_t)size;
}
static ssize_t write_stdout(struct _reent* r,void* fd,const char* p,size_t n) { return write_tee(r,fd,p,n,false); }
static ssize_t write_stderr(struct _reent* r,void* fd,const char* p,size_t n) { return write_tee(r,fd,p,n,true); }
void launcher_log_init(int fd) {
    network=fd;
    saved_stdout=devoptab_list[STD_OUT]; saved_stderr=devoptab_list[STD_ERR];
    tee_stdout=*saved_stdout; tee_stderr=*saved_stderr;
    tee_stdout.name="jvmstdout"; tee_stdout.write_r=write_stdout;
    tee_stderr.name="jvmstderr"; tee_stderr.write_r=write_stderr;
    devoptab_list[STD_OUT]=&tee_stdout; devoptab_list[STD_ERR]=&tee_stderr;
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    if (network>=0) {
        struct timeval timeout={.tv_sec=0,.tv_usec=100000};
        setsockopt(network,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    }
}
void launcher_log_pump(void) {
    pump_native_fault();
    // Avoid the ordinary stdout mutex after a fatal record was published.
    // The faulting thread may hold it; the native fault remains unhandled.
    if (atomic_load_explicit(&native_fault_state,memory_order_acquire)!=0) return;
    if (network<0) return;
    if (pending_at==pending_size) {
        if (!mutexTryLock(&output_mutex)) return;
        pending_size=used<sizeof(pending)?used:sizeof(pending); pending_at=0;
        size_t first=pending_size<QUEUE_SIZE-head?pending_size:QUEUE_SIZE-head;
        memcpy(pending,queue+head,first);
        memcpy(pending+first,queue,pending_size-first);
        head=(head+pending_size)%QUEUE_SIZE; used-=pending_size;
        mutexUnlock(&output_mutex);
    }
    if (pending_size>pending_at) {
        ssize_t count=send(network,pending+pending_at,pending_size-pending_at,0);
        if (count>0) pending_at+=(size_t)count;
        else if (count<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) {
            if (!mutexTryLock(&output_mutex)) return;
            int closing=network; network=-1;
            mutexUnlock(&output_mutex);
            close(closing);
        }
    }
}
void launcher_console_update(void) {
    if (launcher_native_fault_pending() || !mutexTryLock(&output_mutex)) return;
    if (console_enabled) consoleUpdate(NULL);
    mutexUnlock(&output_mutex);
}
void launcher_console_release(void) {
    // Called only by the NRO main/console owner thread. No subsequent writer
    // may touch the console framebuffer after this gate is closed.
    mutexLock(&output_mutex);
    bool active=console_enabled;
    console_enabled=false;
    mutexUnlock(&output_mutex);
    if (active) consoleExit(NULL);
    // libnx consoleExit releases its renderer, not our installed devoptabs.
}
void launcher_console_restore(void) {
    // Only used for a failed startup renderer, before Java threads can write.
    if (console_enabled) return;
    consoleInit(NULL);
    mutexLock(&output_mutex);
    if (devoptab_list[STD_OUT]!=&tee_stdout) {
        saved_stdout=devoptab_list[STD_OUT];
        tee_stdout=*saved_stdout;
        tee_stdout.name="jvmstdout"; tee_stdout.write_r=write_stdout;
        devoptab_list[STD_OUT]=&tee_stdout;
    }
    if (devoptab_list[STD_ERR]!=&tee_stderr) {
        saved_stderr=devoptab_list[STD_ERR];
        tee_stderr=*saved_stderr;
        tee_stderr.name="jvmstderr"; tee_stderr.write_r=write_stderr;
        devoptab_list[STD_ERR]=&tee_stderr;
    }
    console_enabled=true;
    mutexUnlock(&output_mutex);
}
void launcher_log_drain(void) {
    if (launcher_native_fault_pending()) {
        uint64_t start=armGetSystemTick();
        while (atomic_load_explicit(&native_fault_state,memory_order_acquire)!=3 &&
               armTicksToNs(armGetSystemTick()-start)<UINT64_C(500000000)) {
            pump_native_fault();
            svcSleepThread(1000000);
        }
        return;
    }
    mutexLock(&output_mutex); size_t lost=dropped; mutexUnlock(&output_mutex);
    if (lost) fprintf(stderr,"LOG_TEE_DROPPED bytes=%zu\n",lost);
    uint64_t started=armGetSystemTick();
    while (network>=0 && armTicksToNs(armGetSystemTick()-started)<UINT64_C(3000000000)) {
        mutexLock(&output_mutex); size_t queued=used; mutexUnlock(&output_mutex);
        if (!queued && pending_at==pending_size) break;
        launcher_log_pump(); launcher_console_update();
    }
    // HotSpot native daemons can survive DestroyJavaVM. Leave stdio and console
    // installed until svcExitProcess terminates every thread atomically.
}

void horizon_native_exit_flush(void) {
    // Only the supervisor owns the queue consumer and console renderer. An
    // exiting VM/native thread leaves it a bounded opportunity to drain.
    if (threadGetCurHandle()==envGetMainThreadHandle()) launcher_log_drain();
    else svcSleepThread(100000000);
}
