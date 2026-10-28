/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "nx_memory.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

static FILE *network_log;
static unsigned passed, failed;

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
    report("%s: %s (errno=%d rc=0x%08" PRIx32 ")\n", name,
           success ? "PASS" : "FAIL", errno, nxvm_last_result());
    return success;
}

static bool permission_is(void *address, unsigned permission) {
    MemoryInfo info;
    u32 page_info;
    Result result = svcQueryMemory(&info, &page_info, (uintptr_t)address);
    if (R_FAILED(result)) return false;
    return info.perm == permission;
}

static bool bytes_are(const unsigned char *address, size_t bytes, unsigned char value) {
    for (size_t i = 0; i < bytes; ++i) if (address[i] != value) return false;
    return true;
}

static void memory_probe(void) {
    const size_t page = NXVM_PAGE_SIZE;
    u64 resource = 0;
    Result result = svcGetInfo(&resource, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
    report("process.system_resource: rc=0x%08" PRIx32 " bytes=%" PRIu64 "\n", result, resource);
    report("No physical map calls, deliberate access faults, or generated code are used.\n");

    check("reserve.reject_executable", nxvm_reserve(page, page, true) == NULL && errno == ENOTSUP);
    check("reserve.reject_unaligned_size", nxvm_reserve(page + 1, page, false) == NULL && errno == EINVAL);
    check("reserve.reject_alignment", nxvm_reserve(page, 3 * page, false) == NULL && errno == EINVAL);
    unsigned char *base = nxvm_reserve(8 * page, 16 * page, false);
    if (!check("reserve.allocate", base != NULL)) return;
    check("reserve.alignment", !((uintptr_t)base & (16 * page - 1)));
    check("reserve.kernel_none", permission_is(base, Perm_None));
    check("reserve.no_accidental_commit", !nxvm_protect(base, page, NXVM_READ_WRITE) && errno == EINVAL);
    check("commit.reject_outside", !nxvm_commit(base + 8 * page, page, false) && errno == EINVAL);
    check("commit.reject_executable", !nxvm_commit(base, page, true) && errno == ENOTSUP);
    if (!check("commit.first_four", nxvm_commit(base, 4 * page, false))) goto cleanup;
    check("commit.kernel_rw", permission_is(base, Perm_Rw));
    check("commit.tail_still_none", permission_is(base + 4 * page, Perm_None));
    check("commit.zero_initial", bytes_are(base, 4 * page, 0));
    memset(base, 0xa7, 4 * page);
    check("write.roundtrip", bytes_are(base, 4 * page, 0xa7));
    if (!check("protect.read", nxvm_protect(base, page, NXVM_READ))) goto cleanup;
    check("protect.kernel_read", permission_is(base, Perm_R));
    check("protect.read_preserved", bytes_are(base, page, 0xa7));
    if (!check("commit.mixed_old_permissions", nxvm_commit(base, 6 * page, false))) goto cleanup;
    check("commit.preserve_existing", bytes_are(base, 4 * page, 0xa7));
    check("commit.zero_new", bytes_are(base + 4 * page, 2 * page, 0));
    if (!check("uncommit.middle", nxvm_uncommit(base + page, 2 * page))) goto cleanup;
    check("uncommit.kernel_none", permission_is(base + page, Perm_None));
    check("uncommit.neighbor_preserved", bytes_are(base, page, 0xa7) && bytes_are(base + 3 * page, page, 0xa7));
    if (!check("recommit.mixed", nxvm_commit(base, 6 * page, false))) goto cleanup;
    check("recommit.zero_only_discarded", bytes_are(base + page, 2 * page, 0) && bytes_are(base, page, 0xa7));
    if (!check("discard.committed", nxvm_discard(base + 3 * page, page))) goto cleanup;
    check("discard.zero", bytes_are(base + 3 * page, page, 0));
    if (!check("guard.none", nxvm_protect(base, 6 * page, NXVM_NONE))) goto cleanup;
    check("guard.kernel_none", permission_is(base, Perm_None));
    if (!check("unguard.rw", nxvm_protect(base, 6 * page, NXVM_READ_WRITE))) goto cleanup;
    check("unguard.preserved", bytes_are(base, page, 0xa7));
    if (!check("release.prefix", nxvm_release(base, page))) goto cleanup;
    check("release.prefix_kernel_none", permission_is(base, Perm_None));
    check("release.double_rejected", !nxvm_release(base, page) && errno == EINVAL);
    check("commit.released_rejected", !nxvm_commit(base, 2 * page, false) && errno == EINVAL);
    if (!check("release.suffix", nxvm_release(base + 7 * page, page))) {
        check("release.remaining_after_failure", nxvm_release(base + page, 7 * page));
        return;
    }
    struct nxvm_memory_stats stats;
    nxvm_get_memory_stats(&stats);
    check("stats.eager_backing_explicit", stats.reservations == 1 && stats.reserved_bytes == 6 * page &&
          stats.backing_bytes == 8 * page && stats.committed_bytes == 5 * page);
    check("release.trimmed_middle", nxvm_release(base + page, 6 * page));
    nxvm_get_memory_stats(&stats);
    check("release.no_reservations_remain", stats.reservations == 0 && stats.backing_bytes == 0);
    return;
cleanup:
    check("release.cleanup", nxvm_release(base, 8 * page));
}

static void file_probe(void) {
    Result result = romfsInit();
    if (!check("file.romfs", R_SUCCEEDED(result))) return;
    int fd = open("romfs:/memory-fixture.bin", O_RDONLY);
    if (!check("file.open", fd >= 0)) { romfsExit(); return; }
    const size_t bytes = 5000;
    unsigned char *base = nxvm_map_file_private("romfs:/memory-fixture.bin", 17, bytes, true);
    if (check("file.private_snapshot", base != NULL)) {
        bool correct = true;
        for (size_t i = 0; i < bytes; ++i)
            if (base[i] != (unsigned char)((i + 17) * 37 + 13)) { correct = false; break; }
        check("file.content", correct);
        check("file.kernel_read", permission_is(base, Perm_R));
        check("file.tail_zero", bytes_are(base + bytes, 2 * NXVM_PAGE_SIZE - bytes, 0));
        check("file.fd_position_preserved", lseek(fd, 0, SEEK_CUR) == 0);
        check("file.unmap", nxvm_unmap_file(base, bytes));
    }
    check("file.reject_short_read", nxvm_map_file_private("romfs:/memory-fixture.bin", 8000, bytes, true) == NULL && errno == EIO);
    check("file.reject_missing", nxvm_map_file_private("romfs:/missing.bin", 0, bytes, true) == NULL && errno == ENOENT);
    close(fd);
    romfsExit();
    struct nxvm_memory_stats stats;
    nxvm_get_memory_stats(&stats);
    check("file.cleanup_including_errors", stats.reservations == 0 && stats.backing_bytes == 0);
}

int main(void) {
    consoleInit(NULL);
    Result socket_result = socketInitializeDefault();
    if (R_SUCCEEDED(socket_result)) {
        int descriptor = nxlinkConnectToHost(false, false);
        if (descriptor >= 0) {
            network_log = fdopen(descriptor, "w");
            if (!network_log) close(descriptor);
        }
    }
    report("MINESWITCH_MEMORY_PROBE_BEGIN version=0.1.0 applet=%d\n", (int)appletGetAppletType());
    memory_probe();
    file_probe();
    report("MINESWITCH_MEMORY_PROBE_COMPLETE passed=%u failed=%u\n", passed, failed);
    uint64_t start = armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick() - start) < UINT64_C(2000000000))
        consoleUpdate(NULL);
    report("MINESWITCH_MEMORY_PROBE_END passed=%u failed=%u exit=loader\n", passed, failed);
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(socket_result)) socketExit();
    consoleExit(NULL);
    return 0;
}
