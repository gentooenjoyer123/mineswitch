/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "horizon_file_io.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <switch.h>

static FILE *network_log;
static unsigned passed, failed;
static void report(const char *format, ...) {
    va_list args;
    va_start(args, format); vprintf(format, args); va_end(args); fflush(stdout);
    if (network_log) {
        va_start(args, format); vfprintf(network_log, format, args); va_end(args); fflush(network_log);
    }
}
static bool check(const char *name, bool success) {
    if (success) ++passed; else ++failed;
    report("%s: %s errno=%d\n", name, success ? "PASS" : "FAIL", errno);
    return success;
}
static bool content(const unsigned char *data, size_t length, size_t offset) {
    for (size_t i = 0; i < length; ++i)
        if (data[i] != (unsigned char)((i + offset) * 37 + 13)) return false;
    return true;
}
struct worker_state { int fd; bool positioned; unsigned failures; };
static Thread workers[2];
static struct worker_state worker_states[2];
static void worker(void *argument) {
    struct worker_state *state = argument;
    unsigned char data[7];
    for (unsigned i = 0; i < 100; ++i) {
        size_t offset = state->positioned ? 1200 + i * 3 : i * sizeof(data);
        ssize_t count = state->positioned ? nxio_pread(state->fd, data, sizeof(data), offset)
                                          : nxio_read(state->fd, data, sizeof(data));
        if (count != sizeof(data) || !content(data, sizeof(data), offset)) ++state->failures;
    }
}

static bool probe(void) {
    char directory[256], file[300], other[300], alias[300];
    snprintf(directory, sizeof(directory), "sdmc:/switch/mineswitch-io-probe-%016" PRIx64,
             armGetSystemTick());
    snprintf(file, sizeof(file), "%s/MixedCase.bin", directory);
    snprintf(other, sizeof(other), "%s/Distinct.bin", directory);
    snprintf(alias, sizeof(alias), "%s/mixedcase.BIN", directory);
    report("Temporary owned directory: %s\n", directory);
    if (!check("directory.create_unique", mkdir(directory, 0700) == 0)) return true;
    int fd = nxio_open(file, O_CREAT | O_EXCL | O_RDWR, 0600);
    int fd2 = -1;
    bool file_created = fd >= 0, other_created = false;
    if (!check("file.create_exclusive", fd >= 0)) goto cleanup;
    static unsigned char initial[32768];
    unsigned char buffer[512] = {0};
    for (size_t i = 0; i < sizeof(initial); ++i) initial[i] = (unsigned char)(i * 37 + 13);
    if (!check("file.write", nxio_write(fd, initial, sizeof(initial)) == sizeof(initial))) goto cleanup;
    check("file.seek", nxio_seek(fd, 1000, SEEK_SET) == 1000);
    check("file.pread", nxio_pread(fd, buffer, sizeof(buffer), 97) == sizeof(buffer));
    check("file.pread_content", content(buffer, sizeof(buffer), 97));
    check("file.pread_preserves_cursor", nxio_seek(fd, 0, SEEK_CUR) == 1000);
    memset(buffer, 0x88, 256);
    check("file.pwrite", nxio_pwrite(fd, buffer, 256, 4096) == 256);
    check("file.pwrite_preserves_cursor", nxio_seek(fd, 0, SEEK_CUR) == 1000);
    memset(buffer, 0, 256);
    check("file.pwrite_readback", nxio_pread(fd, buffer, 256, 4096) == 256);
    bool correct = true;
    for (size_t i = 0; i < 256; ++i) if (buffer[i] != 0x88) correct = false;
    check("file.pwrite_content", correct);
    struct iovec write_vectors[2] = {{.iov_base="abcdefghijklmnop", .iov_len=16},
                                     {.iov_base="ABCDEFGHIJKLMNOP", .iov_len=16}};
    check("file.vector_seek", nxio_seek(fd, 3000, SEEK_SET) == 3000);
    check("file.writev", nxio_writev(fd, write_vectors, 2) == 32);
    memset(buffer, 0, 32);
    struct iovec read_vectors[2] = {{.iov_base=buffer, .iov_len=11}, {.iov_base=buffer+11, .iov_len=21}};
    check("file.vector_rewind", nxio_seek(fd, 3000, SEEK_SET) == 3000);
    check("file.readv", nxio_readv(fd, read_vectors, 2) == 32);
    check("file.vector_content", memcmp(buffer, "abcdefghijklmnopABCDEFGHIJKLMNOP", 32) == 0);
    struct stat info = {0};
    check("file.fstat", nxio_fstat(fd, &info) == 0 && info.st_size == 32768);
    report("SDK file identity dev=%ju inode=%ju\n", (uintmax_t)info.st_dev, (uintmax_t)info.st_ino);
    check("file.truncate", nxio_truncate(fd, 8192) == 0);
    check("file.truncated_size", nxio_fstat(fd, &info) == 0 && info.st_size == 8192);
    check("file.force", nxio_fsync(fd) == 0);
    struct stat path_info = {0}, alias_info = {0};
    off_t cursor_before_metadata = nxio_seek(fd, 0, SEEK_CUR);
    check("metadata.owned_writer_path", nxio_stat_path(file, &path_info) == 0 &&
          S_ISREG(path_info.st_mode) && path_info.st_size == 8192);
    check("metadata.writer_case_alias", nxio_stat_path(alias, &alias_info) == 0 &&
          alias_info.st_size == path_info.st_size && alias_info.st_mtime == path_info.st_mtime);
    check("metadata.writer_lstat", nxio_lstat_path(alias, &alias_info) == 0 &&
          S_ISREG(alias_info.st_mode) && alias_info.st_size == 8192);
    check("metadata.cursor_preserved", nxio_seek(fd, 0, SEEK_CUR) == cursor_before_metadata);
    report("metadata writer mtime=%jd atime=%jd ctime=%jd\n",
           (intmax_t)path_info.st_mtime, (intmax_t)path_info.st_atime, (intmax_t)path_info.st_ctime);
    unsigned char appended = 0x42;
    check("metadata.append_for_current_size", nxio_pwrite(fd, &appended, 1, 9000) == 1 && nxio_fsync(fd) == 0);
    check("metadata.current_size_after_write", nxio_stat_path(file, &path_info) == 0 && path_info.st_size == 9001);
    check("metadata.restore_size", nxio_truncate(fd, 8192) == 0 && nxio_fsync(fd) == 0);
    check("metadata.current_size_after_truncate", nxio_stat_path(file, &path_info) == 0 && path_info.st_size == 8192);
    check("file.negative_offset_rejected", nxio_pread(fd, buffer, 1, -1) == -1 && errno == EINVAL);

    if (!check("threads.initial_cursor", nxio_seek(fd, 0, SEEK_SET) == 0)) goto cleanup;
    worker_states[0] = (struct worker_state){.fd=fd, .positioned=false};
    worker_states[1] = (struct worker_state){.fd=fd, .positioned=true};
    unsigned created=0, started=0;
    for (unsigned i=0; i<2; ++i) {
        Result rc = threadCreate(&workers[i], worker, &worker_states[i], NULL, 65536, 0x2c, -2);
        if (!check("threads.create", R_SUCCEEDED(rc))) break;
        ++created;
        rc = threadStart(&workers[i]);
        if (!check("threads.start", R_SUCCEEDED(rc))) break;
        ++started;
    }
    for (unsigned i=0; i<started; ++i) {
        Result rc = svcWaitSynchronizationSingle(workers[i].handle, INT64_C(10000000000));
        if (!check("threads.complete_within_10s", R_SUCCEEDED(rc))) return false;
        check("threads.shared_position_content", worker_states[i].failures == 0);
    }
    for (unsigned i=0; i<created; ++i) check("threads.close", R_SUCCEEDED(threadClose(&workers[i])));
    if (started == 2) check("threads.final_cursor", nxio_seek(fd, 0, SEEK_CUR) == 700);
    fd2 = nxio_open(other, O_CREAT | O_EXCL | O_RDWR, 0600);
    other_created = fd2 >= 0;
    if (!check("identity.create_distinct", fd2 >= 0)) goto cleanup;
    check("metadata.distinct_writer_size", nxio_stat_path(other, &path_info) == 0 && path_info.st_size == 0);
    check("metadata.close_original_writer", nxio_close(fd) == 0);
    fd = -1;
    int foreign = open(file, O_WRONLY);
    if (check("metadata.foreign_writer_open", foreign >= 0)) {
        check("metadata.untracked_writer_not_borrowed", nxio_stat_path(file, &path_info) == -1 && errno == EIO);
        check("metadata.foreign_writer_close", close(foreign) == 0);
    }
    fd = nxio_open(file, O_RDWR, 0);
    if (!check("metadata.reopen_tracked_writer", fd >= 0)) goto cleanup;
    check("metadata.reopened_writer", nxio_stat_path(file, &path_info) == 0 && path_info.st_size == 8192);
    char *canonical = nxio_canonical_sd_path(file);
    char *case_alias = nxio_canonical_sd_path(alias);
    char *different = nxio_canonical_sd_path(other);
    check("identity.exact_spelling", canonical && strcmp(canonical, file) == 0);
    check("identity.case_alias_same", canonical && case_alias && strcmp(canonical, case_alias) == 0);
    check("identity.distinct_paths_differ", canonical && different && strcmp(canonical, different) != 0);
    free(canonical); free(case_alias); free(different);
    DIR *dir = opendir(directory);
    if (check("directory.open", dir != NULL)) {
        unsigned entries = 0;
        errno = 0;
        while (readdir(dir) != NULL) ++entries;
        report("directory EOF errno=%d entries=%u\n", errno, entries);
        check("directory.read_entries", entries >= 2);
        check("directory.close", closedir(dir) == 0);
    }
    struct statvfs space;
    if (check("filesystem.statvfs", statvfs(directory, &space) == 0))
        check("filesystem.capacity", space.f_frsize > 0 && space.f_blocks >= space.f_bfree);
cleanup:
    if (fd2 >= 0) check("file.close_distinct", nxio_close(fd2) == 0);
    if (fd >= 0) check("file.close", nxio_close(fd) == 0);
    if (other_created) check("cleanup.distinct", unlink(other) == 0);
    if (file_created) check("cleanup.file", unlink(file) == 0);
    check("cleanup.directory", rmdir(directory) == 0);
    return true;
}

int main(void) {
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    if (R_SUCCEEDED(sockets)) {
        int fd = nxlinkConnectToHost(false, false);
        if (fd >= 0) { network_log = fdopen(fd, "w"); if (!network_log) close(fd); }
    }
    report("MINESWITCH_FILE_IO_BEGIN version=0.2.0 applet=%d\n", (int)appletGetAppletType());
    bool safe = probe();
    report("MINESWITCH_FILE_IO_COMPLETE passed=%u failed=%u\n", passed, failed);
    uint64_t start = armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick()-start) < UINT64_C(2000000000)) consoleUpdate(NULL);
    report("MINESWITCH_FILE_IO_END passed=%u failed=%u exit=%s\n", passed, failed, safe ? "loader" : "process");
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    if (!safe) svcExitProcess();
    return 0;
}
