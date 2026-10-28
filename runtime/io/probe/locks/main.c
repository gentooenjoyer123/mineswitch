/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "horizon_file_io.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void observe(const char *name, Result result) {
    report("OBSERVE %s: rc=0x%08" PRIx32 " module=%u description=%u success=%u\n",
           name, result, R_MODULE(result), R_DESCRIPTION(result), R_SUCCEEDED(result));
}
static Result open_observation(FsFileSystem *filesystem, const char *path,
                              const char *label, u32 mode) {
    FsFile file;
    Result result = fsFsOpenFile(filesystem, path, mode, &file);
    observe(label, result);
    if (R_SUCCEEDED(result)) fsFileClose(&file);
    return result;
}

static void claim_backend_probe(const char *path) {
    /* Minecraft's DirectoryLock uses WRITE without READ. */
    int fd = nxio_open(path, O_WRONLY, 0);
    if (!check("claim.tracked_writer_open", fd >= 0)) return;
    char *key = nxio_file_key(fd);
    check("claim.canonical_string_key", key && strcmp(key, path) == 0);
    free(key);
    check("claim.acquire_real_exclusion", nxio_lock_whole_file(fd) == 0);
    check("claim.duplicate_rejected", nxio_lock_whole_file(fd) == -1 && errno == EAGAIN);
    int contender = nxio_open(path, O_RDWR, 0);
    check("claim.contender_access_denied", contender == -1 && errno == EACCES);
    if (contender >= 0) nxio_close(contender);
    check("claim.release_java_claim", nxio_unlock_whole_file(fd) == 0);
    contender = nxio_open(path, O_RDWR, 0);
    check("claim.mandatory_open_constraint_persists", contender == -1 && errno == EACCES);
    if (contender >= 0) nxio_close(contender);
    check("claim.reacquire_same_channel", nxio_lock_whole_file(fd) == 0);
    check("claim.close_releases_handle", nxio_close(fd) == 0);
    key = nxio_file_key(fd);
    check("claim.closed_key_rejected", key == NULL && errno == EBADF);
    free(key);
    fd = nxio_open(path, O_WRONLY, 0);
    if (check("claim.next_writer_open", fd >= 0)) {
        check("claim.next_writer_acquires", nxio_lock_whole_file(fd) == 0);
        check("claim.next_writer_release", nxio_unlock_whole_file(fd) == 0);
        check("claim.next_writer_close", nxio_close(fd) == 0);
    }
    /* Unknown foreign descriptors must not receive an invented key/claim. */
    fd = open(path, O_RDWR);
    if (check("claim.untracked_writer_open", fd >= 0)) {
        check("claim.untracked_writer_rejected", nxio_lock_whole_file(fd) == -1 && errno == ENOTSUP);
        check("claim.untracked_writer_close", close(fd) == 0);
    }
}

static void probe(void) {
    char directory[256], path[300], renamed[300];
    snprintf(directory, sizeof(directory), "sdmc:/switch/mineswitch-lock-probe-%016" PRIx64, armGetSystemTick());
    snprintf(path, sizeof(path), "%s/Session.bin", directory);
    snprintf(renamed, sizeof(renamed), "%s/Renamed.bin", directory);
    report("Owned temporary directory: %s\n", directory);
    if (!check("directory.create_exclusive", mkdir(directory, 0700) == 0)) return;
    int seed = nxio_open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    bool exists = seed >= 0;
    const char *current = path;
    if (!check("file.create_exclusive", seed >= 0)) goto cleanup;
    static const char payload[] = "MineSwitch lock probe";
    check("file.seed_write", nxio_write(seed, payload, sizeof(payload)) == sizeof(payload));
    check("file.seed_close", nxio_close(seed) == 0);
    FsFileSystem *primary;
    char raw_path[FS_MAX_PATH], raw_renamed[FS_MAX_PATH];
    if (!check("filesystem.translate", fsdevTranslatePath(path, &primary, raw_path) == 0 &&
               fsdevTranslatePath(renamed, NULL, raw_renamed) == 0)) goto cleanup;
    FsFileSystem independent;
    Result result = fsOpenSdCardFileSystem(&independent);
    if (!check("filesystem.independent_session", R_SUCCEEDED(result))) goto cleanup;

    FsFile reader;
    result = fsFsOpenFile(primary, raw_path, FsOpenMode_Read, &reader);
    if (check("readers.first_open", R_SUCCEEDED(result))) {
        open_observation(primary, raw_path, "reader_held.second_reader_same_fs", FsOpenMode_Read);
        open_observation(&independent, raw_path, "reader_held.second_reader_other_fs", FsOpenMode_Read);
        open_observation(&independent, raw_path, "reader_held.writer_other_fs", FsOpenMode_Write);
        fsFileClose(&reader);
    }
    FsFile writer;
    result = fsFsOpenFile(primary, raw_path, FsOpenMode_Write, &writer);
    if (check("writer.first_open", R_SUCCEEDED(result))) {
        Result writer_same = open_observation(primary, raw_path, "writer_held.writer_same_fs", FsOpenMode_Write);
        Result writer_other = open_observation(&independent, raw_path, "writer_held.writer_other_fs", FsOpenMode_Write);
        Result reader_other = open_observation(&independent, raw_path, "writer_held.reader_other_fs", FsOpenMode_Read);
        FsDirEntryType type;
        result = fsFsGetEntryType(&independent, raw_path, &type);
        observe("writer_held.metadata_entry_type", result);
        char *canonical = nxio_canonical_sd_path(path);
        check("writer_held.canonical_metadata", canonical && strcmp(canonical, path) == 0);
        free(canonical);
        struct stat info;
        int status = stat(path, &info);
        report("OBSERVE writer_held.newlib_stat: status=%d errno=%d raw_fs=0x%08" PRIx32 "\n",
               status, errno, fsdevGetLastResult());

        Result rename_result = fsFsRenameFile(&independent, raw_path, raw_renamed);
        observe("writer_held.rename_other_fs", rename_result);
        if (R_SUCCEEDED(rename_result)) current = renamed;
        const char *current_raw = current == renamed ? raw_renamed : raw_path;
        Result delete_result = fsFsDeleteFile(&independent, current_raw);
        observe("writer_held.delete_other_fs", delete_result);
        if (R_SUCCEEDED(delete_result)) exists = false;
        fsFileClose(&writer);
        report("OBSERVATION_SUMMARY other_writer_denied=%u same_writer_denied=%u reader_denied=%u rename_denied=%u delete_denied=%u\n",
               R_FAILED(writer_other), R_FAILED(writer_same), R_FAILED(reader_other),
               R_FAILED(rename_result), R_FAILED(delete_result));
        if (exists) {
            result = open_observation(&independent, current_raw, "writer_closed.reacquire_write", FsOpenMode_Write);
            check("writer_closed.reacquire_succeeds", R_SUCCEEDED(result));
            result = fsFsRenameFile(&independent, current_raw,
                                   current == renamed ? raw_path : raw_renamed);
            observe("writer_closed.rename", result);
            if (R_SUCCEEDED(result)) current = current == renamed ? path : renamed;
        }
    }
    fsFsClose(&independent);
    if (exists) claim_backend_probe(current);
cleanup:
    if (exists) check("cleanup.file", unlink(current) == 0);
    check("cleanup.directory", rmdir(directory) == 0);
}

int main(void) {
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    if (R_SUCCEEDED(sockets)) {
        int fd = nxlinkConnectToHost(false, false);
        if (fd >= 0) { network_log = fdopen(fd, "w"); if (!network_log) close(fd); }
    }
    report("MINESWITCH_FS_LOCK_BEGIN version=0.2.0 applet=%d\n", (int)appletGetAppletType());
    report("Native file-handle observations and lock bridge checks; no Java execution.\n");
    probe();
    report("MINESWITCH_FS_LOCK_COMPLETE passed=%u failed=%u\n", passed, failed);
    uint64_t start = armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick()-start) < UINT64_C(2000000000)) consoleUpdate(NULL);
    report("MINESWITCH_FS_LOCK_END passed=%u failed=%u exit=loader\n", passed, failed);
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    return 0;
}
