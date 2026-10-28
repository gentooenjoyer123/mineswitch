#include "nx_safefetch.h"
#include <inttypes.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <switch.h>

static FILE *network_log;
static unsigned passed, failed;
static atomic_bool workers_start;
struct worker_state { unsigned id; unsigned failures; };
/* These objects outlive a timed-out probe until svcExitProcess stops workers. */
static Thread workers[2];
static struct worker_state worker_states[2] = {{.id=1}, {.id=2}};

static void report(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    if (network_log) {
        va_start(args, format);
        vfprintf(network_log, format, args);
        va_end(args);
        fflush(network_log);
    }
}

static bool check(const char *name, bool success) {
    if (success) ++passed; else ++failed;
    report("%s: %s\n", name, success ? "PASS" : "FAIL");
    return success;
}

static void worker(void *argument) {
    struct worker_state *state = argument;
    while (!atomic_load_explicit(&workers_start, memory_order_acquire))
        svcSleepThread(10000);
    for (unsigned i = 0; i < 2000; ++i) {
        intptr_t fallback = (intptr_t)(UINT64_C(0x1234567800000000) + state->id * 4096 + i);
        if (nxvm_safe_fetch_word(NULL, fallback) != fallback) ++state->failures;
        int32_t small_fallback = -(int32_t)(state->id * 4096 + i + 1);
        if (nxvm_safe_fetch32(NULL, small_fallback) != small_fallback) ++state->failures;
    }
}

static bool probe(void) {
    int32_t small = -1234567;
    intptr_t word = (intptr_t)UINT64_C(0x76543210fedcba98);
    check("read32.valid", nxvm_safe_fetch32(&small, 123) == small);
    check("read_word.valid", nxvm_safe_fetch_word(&word, 123) == word);
    report("Starting actual guarded invalid loads.\n");
    check("read32.null", nxvm_safe_fetch32(NULL, -42) == -42);
    check("read_word.null", nxvm_safe_fetch_word(NULL, word) == word);

    unsigned char *pages = memalign(4096, 8192);
    if (check("protection.allocate", pages != NULL)) {
        *(intptr_t *)pages = word;
        Result rc = svcSetMemoryPermission(pages, 4096, Perm_None);
        if (check("protection.set_none", R_SUCCEEDED(rc))) {
            check("read32.no_access", nxvm_safe_fetch32((int32_t *)pages, -91) == -91);
            check("read_word.no_access", nxvm_safe_fetch_word((intptr_t *)pages, -92) == -92);
            rc = svcSetMemoryPermission(pages, 4096, Perm_Rw);
            if (!check("protection.restore", R_SUCCEEDED(rc))) return false;
            check("read_word.restored", nxvm_safe_fetch_word((intptr_t *)pages, 9) == word);
        }
        free(pages);
    }

    u64 allowed_cores = 0;
    Result core_result = svcGetInfo(&allowed_cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    check("threads.allowed_cores_query", R_SUCCEEDED(core_result));
    report("threads.allowed_cores=0x%" PRIx64 "\n", allowed_cores);
    int assigned_cores[2] = {-2, -2};
    unsigned core_count = 0;
    if (R_SUCCEEDED(core_result)) {
        for (unsigned core = 0; core < 32 && core_count < 2; ++core)
            if (allowed_cores & (UINT64_C(1) << core)) assigned_cores[core_count++] = (int)core;
    }
    if (core_count == 1) assigned_cores[1] = assigned_cores[0];
    report("threads.assigned_cores=%d,%d parallel_cores=%s\n", assigned_cores[0],
           assigned_cores[1], core_count >= 2 ? "yes" : "unavailable");
    unsigned created = 0, started = 0;
    for (unsigned i = 0; i < 2; ++i) {
        Result rc = threadCreate(&workers[i], worker, &worker_states[i], NULL, 64 * 1024, 0x2c, -2);
        if (!check("threads.create", R_SUCCEEDED(rc))) break;
        ++created;
        if (assigned_cores[i] >= 0) {
            rc = svcSetThreadCoreMask(workers[i].handle, assigned_cores[i],
                                      UINT32_C(1) << assigned_cores[i]);
            if (!check("threads.assign_permitted_core", R_SUCCEEDED(rc))) break;
        }
        rc = threadStart(&workers[i]);
        if (!check("threads.start", R_SUCCEEDED(rc))) break;
        ++started;
    }
    atomic_store_explicit(&workers_start, true, memory_order_release);
    for (unsigned i = 0; i < started; ++i) {
        Result rc = svcWaitSynchronizationSingle(workers[i].handle, INT64_C(10000000000));
        if (!check("threads.finish_within_10s", R_SUCCEEDED(rc))) return false;
        check("threads.fault_recovery_isolated", worker_states[i].failures == 0);
    }
    bool closed = true;
    for (unsigned i = 0; i < created; ++i)
        if (!check("threads.close", R_SUCCEEDED(threadClose(&workers[i])))) closed = false;
    return closed;
}

int main(void) {
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    if (R_SUCCEEDED(sockets)) {
        int descriptor = nxlinkConnectToHost(false, false);
        if (descriptor >= 0) {
            network_log = fdopen(descriptor, "w");
            if (!network_log) close(descriptor);
        }
    }
    report("MINESWITCH_SAFEFETCH_BEGIN version=0.1.0 applet=%d\n", (int)appletGetAppletType());
    bool safe_to_return = probe();
    report("MINESWITCH_SAFEFETCH_COMPLETE passed=%u failed=%u\n", passed, failed);
    uint64_t start = armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick()-start) < UINT64_C(2000000000))
        consoleUpdate(NULL);
    report("MINESWITCH_SAFEFETCH_END passed=%u failed=%u exit=%s\n", passed, failed,
           safe_to_return ? "loader" : "process");
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    if (!safe_to_return) svcExitProcess();
    return 0;
}
