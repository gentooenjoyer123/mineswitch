#include "nx_jit.h"
#include "nx_memory.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <switch.h>

typedef uint64_t (*CodeFunction)(void);
static FILE *network_log;
static unsigned passed, failed;
static Thread worker_thread;
static struct {
    CodeFunction function;
    atomic_int phase;
    uint64_t first, second;
    unsigned core;
} worker;
static void report(const char *format, ...) {
    va_list args;
    va_start(args, format); vprintf(format, args); va_end(args); fflush(stdout);
    if (network_log) {
        va_start(args, format); vfprintf(network_log, format, args); va_end(args); fflush(network_log);
    }
}
static bool check(const char *name, bool success) {
    if (success) ++passed; else ++failed;
    report("%s: %s\n", name, success ? "PASS" : "FAIL");
    return success;
}
static bool permission(void *address, unsigned expected) {
    MemoryInfo info;
    u32 page;
    return R_SUCCEEDED(svcQueryMemory(&info, &page, (uintptr_t)address)) && info.perm == expected;
}
static void run_other_core(void *unused) {
    (void)unused;
    worker.core = svcGetCurrentProcessorNumber();
    worker.first = worker.function();
    atomic_store(&worker.phase, 1);
    uint64_t start = armGetSystemTick();
    while (atomic_load(&worker.phase) != 2 &&
           armTicksToNs(armGetSystemTick()-start) < UINT64_C(3000000000)) svcSleepThread(1000000);
    worker.second = worker.function();
    atomic_store(&worker.phase, 3);
}
static bool probe(void) {
    NxJitArena *arena = nxjit_create(128 * 1024, 64 * 1024);
    if (!check("arena.create", arena != NULL)) { report("arena failure=0x%08" PRIx32 "\n", nxjit_last_result()); return true; }
    unsigned char *rw = nxjit_rw_address(arena), *rx = nxjit_rx_address(arena);
    report("CodeMemory RW=%p RX=%p bytes=%zu\n", rw, rx, nxjit_size(arena));
    check("arena.distinct_aliases", rw != rx);
    check("arena.requested_alignment", ((uintptr_t)rw & 65535) == 0);
    check("arena.kernel_rw", permission(rw, Perm_Rw));
    check("arena.kernel_rx", permission(rx, Perm_Rx));
    check("arena.translate_both_directions", nxjit_executable_address(rw + 17) == rx + 17 &&
          nxjit_writable_address(rx + 17) == rw + 17);
    check("arena.translate_idempotent", nxjit_writable_address(rw + 17) == rw + 17 &&
          nxjit_executable_address(rx + 17) == rx + 17);
    check("arena.end_bound", nxjit_executable_address(rw + nxjit_size(arena)) == rx + nxjit_size(arena));
    int temporary = 0;
    check("arena.temporary_identity", nxjit_writable_address(&temporary) == &temporary &&
          nxjit_executable_address(&temporary) == &temporary);
    check("arena.range_checks", nxjit_contains(rx, nxjit_size(arena)) &&
          !nxjit_contains(rx + nxjit_size(arena) - 1, 2));

    // Generated AArch64 leaf: mov x0,#42; ret. Only RW is written.
    ((uint32_t*)rw)[0] = 0xd2800540;
    ((uint32_t*)rw)[1] = 0xd65f03c0;
    nxjit_flush_instruction_cache(rx, 8);
    CodeFunction function = (CodeFunction)(void*)rx;
    check("execute.return42", function() == 42);
    // ADR x0, current instruction verifies the CPU sees the RX PC.
    ((uint32_t*)(rw + 64))[0] = 0x10000000;
    ((uint32_t*)(rw + 64))[1] = 0xd65f03c0;
    nxjit_flush_instruction_cache(rw + 64, 8);
    check("execute.actual_rx_pc", ((CodeFunction)(void*)(rx + 64))() == (uintptr_t)(rx + 64));
    // PC-relative branch within executable space.
    *(uint32_t*)(rw + 128) = 0x14000008; // b +32
    *(uint32_t*)(rw + 160) = 0xd2800c60; // mov x0,#99
    *(uint32_t*)(rw + 164) = 0xd65f03c0;
    nxjit_flush_instruction_cache(rx + 128, 40);
    check("execute.pc_relative_branch", ((CodeFunction)(void*)(rx + 128))() == 99);

    // RX instructions load a literal RW metadata address, then load its value.
    *(uint64_t*)(rw + 256) = UINT64_C(0x123456789abcdef0);
    *(uint32_t*)(rw + 512) = 0x58000081; // ldr x1,[pc,#16]
    *(uint32_t*)(rw + 516) = 0xf9400020; // ldr x0,[x1]
    *(uint32_t*)(rw + 520) = 0xd65f03c0;
    *(uint32_t*)(rw + 524) = 0xd503201f;
    *(uint64_t*)(rw + 528) = (uintptr_t)(rw + 256);
    nxjit_flush_instruction_cache(rx + 512, 24);
    CodeFunction metadata = (CodeFunction)(void*)(rx + 512);
    check("execute.rw_metadata", metadata() == UINT64_C(0x123456789abcdef0));
    *(uint64_t*)(rw + 256) = UINT64_C(0xfedcba9876543210);
    check("execute.mutable_metadata", metadata() == UINT64_C(0xfedcba9876543210));

    u64 core_mask = 0;
    Result result = svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int core = -2;
    unsigned current = svcGetCurrentProcessorNumber();
    for (unsigned i=0; i<4; ++i) if ((core_mask & (UINT64_C(1) << i)) && i != current) { core = (int)i; break; }
    check("publish.other_core_available", R_SUCCEEDED(result) && core >= 0);
    worker.function = function;
    atomic_store(&worker.phase, 0);
    result = threadCreate(&worker_thread, run_other_core, NULL, NULL, 65536, 0x2c, core);
    if (check("publish.worker_create", R_SUCCEEDED(result))) {
        result = threadStart(&worker_thread);
        if (check("publish.worker_start", R_SUCCEEDED(result))) {
            uint64_t start = armGetSystemTick();
            while (atomic_load(&worker.phase) != 1 &&
                   armTicksToNs(armGetSystemTick()-start) < UINT64_C(2000000000)) svcSleepThread(1000000);
            check("publish.worker_warmed", atomic_load(&worker.phase) == 1);
            atomic_store_explicit((_Atomic uint32_t*)nxjit_writable_address(rx), 0xd2800560, memory_order_release);
            nxjit_flush_instruction_cache(rx, 4);
            atomic_store(&worker.phase, 2);
            result = svcWaitSynchronizationSingle(worker_thread.handle, INT64_C(4000000000));
            if (!check("publish.worker_finished", R_SUCCEEDED(result))) return false;
            report("Worker core=%u main core=%u first=%" PRIu64 " second=%" PRIu64 "\n",
                   worker.core, current, worker.first, worker.second);
            check("publish.other_core_repatch", worker.core != current && worker.first == 42 && worker.second == 43);
            check("publish.local_repatch", function() == 43);
        }
        check("publish.worker_close", R_SUCCEEDED(threadClose(&worker_thread)));
    }

    Thread *self = threadGetSelf();
    unsigned char *bottom = self->stack_mirror;
    if (check("stack.guard_none", nxvm_protect_current_stack(bottom, 8192, NXVM_NONE))) {
        check("stack.kernel_none", permission(bottom, Perm_None));
        check("stack.partial_unguard", nxvm_protect_current_stack(bottom + 4096, 4096, NXVM_READ_WRITE));
        check("stack.mixed_state", permission(bottom, Perm_None) && permission(bottom + 4096, Perm_Rw));
        if (!check("stack.restore_mixed", nxvm_protect_current_stack(bottom, 8192, NXVM_READ_WRITE))) return false;
    }
    check("stack.reject_live_sp", !nxvm_protect_current_stack((void*)((uintptr_t)&temporary & ~(uintptr_t)4095), 4096, NXVM_NONE));
    check("stack.reject_code_arena", !nxvm_protect_current_stack(rw, 4096, NXVM_NONE));
    check("arena.destroy_quiescent", nxjit_destroy(arena));
    check("arena.removed_from_registry", !nxjit_contains(rx, 1));
    return true;
}
int main(void) {
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    if (R_SUCCEEDED(sockets)) {
        int fd = nxlinkConnectToHost(false, false);
        if (fd >= 0) { network_log = fdopen(fd, "w"); if (!network_log) close(fd); }
    }
    report("MINESWITCH_JIT_MEMORY_BEGIN version=0.1.0 applet=%d\n", (int)appletGetAppletType());
    bool safe = probe();
    report("MINESWITCH_JIT_MEMORY_COMPLETE passed=%u failed=%u\n", passed, failed);
    if (!safe) { report("MINESWITCH_JIT_MEMORY_END exit=process\n"); svcExitProcess(); }
    uint64_t start = armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick()-start) < UINT64_C(2000000000)) consoleUpdate(NULL);
    report("MINESWITCH_JIT_MEMORY_END passed=%u failed=%u exit=loader\n", passed, failed);
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    return 0;
}
