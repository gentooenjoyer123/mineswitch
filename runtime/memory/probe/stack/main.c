#include "nx_memory.h"
#include "nx_safefetch.h"
#include "horizon_aarch64_context.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <switch.h>

extern void nxvm_stack_test_store(void *address, uint32_t value);
extern char nxvm_stack_test_store_pc[], nxvm_stack_test_store_resume[];
static FILE *network_log;
static Thread worker;
static unsigned passed, failed;
static bool worker_returned;
static _Thread_local void *fault_page;
static _Thread_local unsigned fault_calls;
static _Thread_local bool fault_remapped;
static _Thread_local uint32_t fault_result;

static void report(const char *format, ...) {
    va_list args;
    va_start(args, format); vprintf(format, args); va_end(args); fflush(stdout);
    if (network_log) {
        va_start(args, format); vfprintf(network_log, format, args); va_end(args); fflush(network_log);
    }
}
static bool check(const char *name, bool success) {
    if (success) ++passed; else ++failed;
    report("stack.guard.check %s %s errno=%d raw=0x%08" PRIx32 "\n", name,
           success ? "PASS" : "FAIL", errno, nxvm_last_result());
    return success;
}
static void mapping(const char *name, void *address) {
    MemoryInfo info = {0}; u32 page;
    Result query = svcQueryMemory(&info, &page, (uintptr_t)address);
    report("%s query=0x%08" PRIx32 " base=%p bytes=%" PRIu64 " type=%u attr=%u perm=%u\n",
           name, query, (void*)info.addr, info.size, info.type, info.attr, info.perm);
}

/* Exact test-store PC and current thread's registered guard page only. A
 * successful remap retries the real store. Failure skips that test instruction
 * only, records a failed check, and lets ordinary cleanup produce diagnostics. */
int horizon_aarch64_handle_exception(HorizonAArch64Context *context) {
    if (!fault_page || context->pc != (uintptr_t)nxvm_stack_test_store_pc ||
        context->far != (uintptr_t)fault_page || (context->esr >> 26) != 0x24 ||
        !(context->esr & (UINT32_C(1) << 6))) return 0;
    ++fault_calls;
    fault_remapped = nxvm_protect_current_stack(fault_page, NXVM_PAGE_SIZE, NXVM_READ_WRITE);
    fault_result = nxvm_last_result();
    if (!fault_remapped) context->pc = (uintptr_t)nxvm_stack_test_store_resume;
    return 1;
}

static void worker_entry(void *ignored) {
    (void)ignored;
    Thread *self = threadGetSelf();
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    report("worker self=%p backing=%p mirror=%p usable_bytes=%zu SP=%p\n", self,
           self->stack_mem, self->stack_mirror, self->stack_sz, (void*)sp);
    mapping("worker.mirror", self->stack_mirror);
    mapping("worker.backing", self->stack_mem);
    if (!check("exception_stack", horizon_aarch64_exception_thread_init() == 0)) goto done;
    if (!check("prepare_owned_pair", nxvm_stack_guard_prepare())) goto exception_done;
    unsigned char *bottom = self->stack_mirror;
    *(uint32_t*)bottom = UINT32_C(0x12345678);
    *(uint32_t*)(bottom + NXVM_PAGE_SIZE) = UINT32_C(0x76543210);
    if (!check("unmap_two_guard_pages", nxvm_protect_current_stack(bottom, 8192, NXVM_NONE))) goto cleanup;
    mapping("worker.after_guard", bottom);
    check("guard_read_faults", nxvm_safe_fetch32((int32_t*)bottom, -31) == -31);
    check("second_guard_read_faults", nxvm_safe_fetch32((int32_t*)(bottom + NXVM_PAGE_SIZE), -32) == -32);
    check("idempotent_guard", nxvm_protect_current_stack(bottom, 8192, NXVM_NONE));
    if (!check("partial_remap", nxvm_protect_current_stack(bottom + NXVM_PAGE_SIZE, 4096, NXVM_READ_WRITE))) goto cleanup;
    check("red_page_still_guarded", nxvm_safe_fetch32((int32_t*)bottom, -33) == -33);
    check("yellow_data_preserved", *(uint32_t*)(bottom + NXVM_PAGE_SIZE) == UINT32_C(0x76543210));
    if (!check("mixed_region_remap", nxvm_protect_current_stack(bottom, 8192, NXVM_READ_WRITE))) goto cleanup;
    check("red_data_preserved", *(uint32_t*)bottom == UINT32_C(0x12345678));
    check("read_only_explicitly_unsupported", !nxvm_protect_current_stack(bottom, 4096, NXVM_READ) && errno == ENOTSUP);
    check("reject_active_stack", !nxvm_protect_current_stack((void*)(sp & ~(uintptr_t)4095), 4096, NXVM_NONE));
    check("reject_backing_alias", !nxvm_protect_current_stack(self->stack_mem, 4096, NXVM_NONE));
    if (!check("reguard_for_native_fault", nxvm_protect_current_stack(bottom, 4096, NXVM_NONE))) goto cleanup;
    fault_page = bottom;
    nxvm_stack_test_store(bottom, UINT32_C(0x24681357));
    fault_page = NULL;
    report("worker.fault calls=%u remapped=%d raw=0x%08" PRIx32 "\n", fault_calls, fault_remapped, fault_result);
    check("remap_inside_exception", fault_calls == 1 && fault_remapped);
    check("fault_store_retried", nxvm_safe_fetch32((int32_t*)bottom, -34) == (int32_t)UINT32_C(0x24681357));
    check("guard_before_cleanup", nxvm_protect_current_stack(bottom, 8192, NXVM_NONE));
cleanup:
    if (!check("cleanup_restores_all_holes", nxvm_stack_guard_cleanup())) {
        report("STACK_GUARD_UNSAFE_EXIT\n"); svcExitProcess();
    }
    mapping("worker.after_cleanup", self->stack_mirror);
    check("cleaned_mapping_readable", nxvm_safe_fetch32((int32_t*)bottom, -35) != -35);
    check("reprepare", nxvm_stack_guard_prepare());
    check("second_cleanup", nxvm_stack_guard_cleanup());
exception_done:
    horizon_aarch64_exception_thread_exit();
done:
    worker_returned = true;
}

int main(void) {
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    if (R_SUCCEEDED(sockets)) {
        int fd = nxlinkConnectToHost(false, false);
        if (fd >= 0) { network_log = fdopen(fd, "w"); if (!network_log) close(fd); }
    }
    report("MINESWITCH_STACK_GUARD_BEGIN version=0.2.0 map_unmap=1\n");
    Thread *self = threadGetSelf();
    report("main backing=%p mirror=%p usable_bytes=%zu\n", self->stack_mem, self->stack_mirror, self->stack_sz);
    check("loader_stack_rejected", !nxvm_stack_guard_prepare() && errno == ENOTSUP && self->stack_mem == NULL);
    Result result = threadCreate(&worker, worker_entry, NULL, NULL, 4 * 1024 * 1024, 0x3b, -2);
    if (check("worker_create", R_SUCCEEDED(result))) {
        result = threadStart(&worker);
        if (check("worker_start", R_SUCCEEDED(result))) {
            result = svcWaitSynchronizationSingle(worker.handle, INT64_C(10000000000));
            if (!check("worker_finish", R_SUCCEEDED(result))) svcExitProcess();
            check("worker_returned", worker_returned);
        }
        check("sdk_full_stack_close", R_SUCCEEDED(threadClose(&worker)));
    }
    report("MINESWITCH_STACK_GUARD_RESULT passed=%u failed=%u\n", passed, failed);
    uint64_t start = armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick()-start) < UINT64_C(1000000000)) consoleUpdate(NULL);
    report("MINESWITCH_STACK_GUARD_END exit=loader\n");
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    return failed ? 1 : 0;
}
