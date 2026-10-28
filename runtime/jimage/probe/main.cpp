#include "jni.h"
#include "osSupport.hpp"
#include "nx_memory.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <switch.h>

static FILE* network_log;
static unsigned passed, failed;

static void report(const char* format, ...) {
    va_list args;
    va_start(args, format); vprintf(format, args); va_end(args); fflush(stdout);
    if (network_log) {
        va_start(args, format); vfprintf(network_log, format, args); va_end(args); fflush(network_log);
    }
}
static bool check(const char* name, bool success) {
    if (success) ++passed; else ++failed;
    report("%s: %s\n", name, success ? "PASS" : "FAIL");
    return success;
}
static bool content_correct(const unsigned char* bytes, size_t count, size_t offset) {
    for (size_t i = 0; i < count; ++i)
        if (bytes[i] != (unsigned char)((i + offset) * 37 + 13)) return false;
    return true;
}
struct Worker { int fd; unsigned id; unsigned failures; };
static void read_worker(void* argument) {
    Worker* worker = (Worker*)argument;
    unsigned char bytes[257];
    for (unsigned i = 0; i < 100; ++i) {
        size_t offset = (i * 71 + worker->id * 997) % 7000;
        if (osSupport::read(worker->fd, (char*)bytes, sizeof(bytes), offset) != sizeof(bytes) ||
            !content_correct(bytes, sizeof(bytes), offset)) ++worker->failures;
    }
}

static bool probe(void) {
    Result result = romfsInit();
    if (!check("romfs.init", R_SUCCEEDED(result))) return true;
    int fd = osSupport::openReadOnly("romfs:/memory-fixture.bin");
    if (!check("image.open", fd >= 0)) { romfsExit(); return true; }
    check("image.size", osSupport::size("romfs:/memory-fixture.bin") == 8195);
    unsigned char bytes[257];
    check("image.positioned_read", osSupport::read(fd, (char*)bytes, sizeof(bytes), 17) == sizeof(bytes));
    check("image.read_content", content_correct(bytes, sizeof(bytes), 17));
    check("image.fd_cursor_restored", lseek(fd, 0, SEEK_CUR) == 0);
    check("image.negative_offset", osSupport::read(fd, (char*)bytes, sizeof(bytes), -1) == -1 && errno == EINVAL);
    check("image.invalid_descriptor", osSupport::read(-1, (char*)bytes, sizeof(bytes), 0) == -1 && errno == EBADF);
    check("image.short_read", osSupport::read(fd, (char*)bytes, sizeof(bytes), 8190) == 5);
    check("image.short_read_content", content_correct(bytes, 5, 8190));

    void* snapshot = osSupport::map_memory(fd, "romfs:/name-is-not-used.bin", 37, 5000);
    if (check("image.map_original_fd", snapshot != NULL)) {
        check("image.map_content", content_correct((unsigned char*)snapshot, 5000, 37));
        MemoryInfo info;
        u32 page;
        result = svcQueryMemory(&info, &page, (uintptr_t)snapshot);
        check("image.map_kernel_readonly", R_SUCCEEDED(result) && info.perm == Perm_R);
        check("image.unmap", osSupport::unmap_memory(snapshot, 5000) == 1);
    }
    check("image.map_short_rejected", osSupport::map_memory(fd, NULL, 8000, 5000) == NULL && errno == EIO);
    nxvm_memory_stats stats;
    nxvm_get_memory_stats(&stats);
    check("image.mapping_cleanup", stats.reservations == 0 && stats.backing_bytes == 0);

    // Retain ownership records if timeout forces process exit after log flush.
    static Thread threads[2];
    static Worker workers[2];
    workers[0] = {fd, 0, 0};
    workers[1] = {fd, 1, 0};
    unsigned created = 0, started = 0;
    for (unsigned i = 0; i < 2; ++i) {
        result = threadCreate(&threads[i], read_worker, &workers[i], NULL, 65536, 0x2c, -2);
        if (!check("threads.create", R_SUCCEEDED(result))) break;
        ++created;
        result = threadStart(&threads[i]);
        if (!check("threads.start", R_SUCCEEDED(result))) break;
        ++started;
    }
    for (unsigned i = 0; i < started; ++i) {
        result = svcWaitSynchronizationSingle(threads[i].handle, INT64_C(10000000000));
        if (!check("threads.complete_within_10s", R_SUCCEEDED(result))) return false;
        check("threads.shared_fd_positioned_content", workers[i].failures == 0);
    }
    for (unsigned i = 0; i < created; ++i) check("threads.close", R_SUCCEEDED(threadClose(&threads[i])));
    check("threads.fd_cursor_restored", lseek(fd, 0, SEEK_CUR) == 0);
    check("image.close", osSupport::close(fd) == 0);
    check("image.closed_read_rejected", osSupport::read(fd, (char*)bytes, 1, 0) == -1 && errno == EBADF);
    check("image.double_close_rejected", osSupport::close(fd) == -1 && errno == EBADF);
    romfsExit();
    return true;
}

int main(void) {
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    if (R_SUCCEEDED(sockets)) {
        int fd = nxlinkConnectToHost(false, false);
        if (fd >= 0) { network_log = fdopen(fd, "w"); if (!network_log) close(fd); }
    }
    report("MINESWITCH_JIMAGE_IO_BEGIN version=0.1.0 applet=%d\n", (int)appletGetAppletType());
    report("Real libjimage Horizon OS adapter; no JVM or class execution.\n");
    bool safe_to_return = probe();
    report("MINESWITCH_JIMAGE_IO_COMPLETE passed=%u failed=%u\n", passed, failed);
    uint64_t start = armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick() - start) < UINT64_C(2000000000))
        consoleUpdate(NULL);
    report("MINESWITCH_JIMAGE_IO_END passed=%u failed=%u exit=%s\n", passed, failed,
           safe_to_return ? "loader" : "process");
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    if (!safe_to_return) svcExitProcess();
    return 0;
}
