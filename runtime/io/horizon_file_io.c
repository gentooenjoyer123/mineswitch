/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "horizon_file_io.h"
#include "horizon_file_share.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <switch/kernel/mutex.h>
#include <switch/runtime/devices/fs_dev.h>
#include <switch/services/time.h>

/* Fixed storage avoids allocating or acquiring malloc locks during close/error
 * handling. Stripes may serialize unrelated regular files, never weaken safety. */
static Mutex descriptor_locks[64];
enum { NXIO_MAX_DESCRIPTORS = 1024 };
/* devkitPro/newlib handle_manager.c at the pinned SDK source defines 1024.
 * Installed libsysbase.a confirms handles[0x2000/8] and __get_handle rejects
 * indices above 0x3ff. This is the actual descriptor table, not OPEN_MAX's
 * unrelated POSIX minimum. Revalidate when upgrading the pinned SDK. */
int nxio_descriptor_limit(void) { return NXIO_MAX_DESCRIPTORS; }
static Mutex *fd_lock(int fd) { return &descriptor_locks[(unsigned)fd % 64]; }
static void unlock_preserving_errno(Mutex *lock) {
    int error = errno; mutexUnlock(lock); errno = error;
}
int nxio_open(const char *path, int flags, int mode) {
    int handled = 0;
    int fd = nxio_shared_open(path, flags, mode, &handled);
    if (!handled) fd = open(path, flags, mode);
    if (fd < 0) {
        /* fsdev otherwise turns TargetLocked into generic EIO. Minecraft's
         * DirectoryLock handles AccessDeniedException as an occupied world. */
        int error = errno;
        if (error == EIO && fsdevGetLastResult() == 0xe02) {
            FsFileSystem *filesystem = NULL;
            char translated[FS_MAX_PATH];
            if (fsdevTranslatePath(path, &filesystem, translated) == 0 &&
                filesystem == fsdevGetDeviceFileSystem("sdmc")) error = EACCES;
        }
        errno = error;
        return -1;
    }
    return fd;
}
int nxio_close(int fd) {
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    int result = close(fd);
    unlock_preserving_errno(lock); return result;
}
ssize_t nxio_read(int fd, void *buffer, size_t size) {
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    void *shared = nxio_shared_begin(fd);
    ssize_t result = read(fd, buffer, size);
    nxio_shared_end(shared); unlock_preserving_errno(lock); return result;
}
ssize_t nxio_write(int fd, const void *buffer, size_t size) {
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    void *shared = nxio_shared_begin(fd);
    ssize_t result = write(fd, buffer, size);
    nxio_shared_end(shared); unlock_preserving_errno(lock); return result;
}
static ssize_t positioned(int fd, void *buffer, size_t size, off_t offset, int writing) {
    if (offset < 0 || size > INT64_MAX || (uint64_t)offset > INT64_MAX - size) {
        errno = EINVAL; return -1;
    }
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    void *shared = nxio_shared_begin(fd);
    off_t original = lseek(fd, 0, SEEK_CUR);
    if (original < 0 || lseek(fd, offset, SEEK_SET) < 0) {
        nxio_shared_end(shared); unlock_preserving_errno(lock); return -1;
    }
    ssize_t result = writing ? write(fd, buffer, size) : read(fd, buffer, size);
    int error = errno;
    if (lseek(fd, original, SEEK_SET) < 0) { result = -1; error = errno; }
    errno = error; nxio_shared_end(shared); unlock_preserving_errno(lock); return result;
}
ssize_t nxio_pread(int fd, void *buffer, size_t size, off_t offset) {
    return positioned(fd, buffer, size, offset, 0);
}
ssize_t nxio_pwrite(int fd, const void *buffer, size_t size, off_t offset) {
    return positioned(fd, (void*)buffer, size, offset, 1);
}
static ssize_t vectored(int fd, const struct iovec *iov, int count, int writing) {
    if (count < 0 || count > 1024 || (count && !iov)) { errno = EINVAL; return -1; }
    size_t size = 0;
    for (int i = 0; i < count; ++i) {
        if (iov[i].iov_len > INT64_MAX - size) { errno = EINVAL; return -1; }
        size += iov[i].iov_len;
    }
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    void *shared = nxio_shared_begin(fd);
    ssize_t total = 0;
    for (int i = 0; i < count; ++i) {
        if (!iov[i].iov_len) continue;
        ssize_t n = writing ? write(fd, iov[i].iov_base, iov[i].iov_len) :
                              read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) { if (!total) total = -1; break; }
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    nxio_shared_end(shared); unlock_preserving_errno(lock); return total;
}
ssize_t nxio_readv(int fd, const struct iovec *iov, int count) { return vectored(fd, iov, count, 0); }
ssize_t nxio_writev(int fd, const struct iovec *iov, int count) { return vectored(fd, iov, count, 1); }
off_t nxio_seek(int fd, off_t offset, int origin) {
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    off_t result = lseek(fd, offset, origin); unlock_preserving_errno(lock); return result;
}
int nxio_fsync(int fd) {
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    int result = fsync(fd); unlock_preserving_errno(lock); return result;
}
int nxio_truncate(int fd, off_t length) {
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    int result = ftruncate(fd, length); unlock_preserving_errno(lock); return result;
}

/* Match fsdev's filesystem timestamp conversion, using a thread-local struct
 * instead of gmtime's shared result. If the timezone service is unavailable,
 * fsdev itself leaves the returned filesystem timestamp unadjusted. */
static time_t filesystem_timestamp(u64 timestamp) {
    time_t result = (time_t)timestamp;
    struct tm calendar;
    if (gmtime_r(&result, &calendar) != NULL) {
        TimeCalendarTime local = {0};
        local.year = 1900 + calendar.tm_year;
        local.month = 1 + calendar.tm_mon;
        local.day = calendar.tm_mday;
        local.hour = calendar.tm_hour;
        local.minute = calendar.tm_min;
        local.second = calendar.tm_sec;
        u64 converted = timestamp;
        s32 count = 0;
        if (R_SUCCEEDED(timeToPosixTimeWithMyRule(&local, &converted, 1, &count)) && count > 0)
            result = (time_t)converted;
    }
    return result;
}

/* Caller owns the descriptor stripe. libnx fstat supplies genuine handle
 * metadata but caches timestamps at open; refresh available timestamps through
 * the filesystem metadata command, which does not open a second data handle.
 * If that optional command cannot supply them, retain exactly fstat's values. */
static void refresh_path_timestamps(const char *path, struct stat *info) {
    int saved_errno = errno;
    FsFileSystem *filesystem;
    char translated[FS_MAX_PATH];
    if (fsdevTranslatePath(path, &filesystem, translated) == 0) {
        FsTimeStampRaw timestamps = {0};
        if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(filesystem, translated, &timestamps)) && timestamps.is_valid) {
            info->st_ctim.tv_sec = filesystem_timestamp(timestamps.created);
            info->st_mtim.tv_sec = filesystem_timestamp(timestamps.modified);
            info->st_atim.tv_sec = filesystem_timestamp(timestamps.accessed);
            // The native metadata API has second precision.
            info->st_ctim.tv_nsec = info->st_mtim.tv_nsec = info->st_atim.tv_nsec = 0;
        }
    }
    errno = saved_errno;
}
int nxio_fstat(int fd, struct stat *info) {
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    int result = fstat(fd, info);
    if (result == 0) {
        int error = errno;
        char *path = nxio_shared_path(fd);
        if (path) { refresh_path_timestamps(path, info); free(path); }
        errno = error;
    }
    unlock_preserving_errno(lock); return result;
}

static int stat_path(const char *path, struct stat *info, int follow_links) {
    int result = follow_links ? stat(path, info) : lstat(path, info);
    if (result == 0 || errno != EIO || fsdevGetLastResult() != 0xe02) return result;
    int original_error = errno;
    /* Resolve before taking the shared registry. Its native backing lifetime
     * is owned by devoptab close, including ordinary libc fclose callers. */
    char *canonical = nxio_canonical_sd_path(path);
    if (!canonical) { errno = original_error; return -1; }
    int found = 0;
    result = nxio_shared_stat_path(canonical, info, &found);
    if (found) {
        if (result == 0) refresh_path_timestamps(canonical, info);
        int error = errno;
        free(canonical); errno = error; return result;
    }
    free(canonical);
    // A writer may have closed after the first attempt. Retry the real path
    // operation once; a foreign writer or another failure remains an error.
    return follow_links ? stat(path, info) : lstat(path, info);
}
int nxio_stat_path(const char *path, struct stat *info) { return stat_path(path, info, 1); }
int nxio_lstat_path(const char *path, struct stat *info) { return stat_path(path, info, 0); }

char *nxio_file_key(int fd) {
    if (fd < 0 || fd >= NXIO_MAX_DESCRIPTORS) { errno = EBADF; return NULL; }
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    char *key = NULL;
    struct stat info;
    if (fstat(fd, &info) == 0) key = nxio_shared_path(fd);
    unlock_preserving_errno(lock);
    return key;
}

int nxio_lock_whole_file(int fd) {
    if (fd < 0 || fd >= NXIO_MAX_DESCRIPTORS) { errno = EBADF; return -1; }
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    void *shared = nxio_shared_begin(fd);
    char *canonical = NULL;
    struct stat info;
    int result = -1;
    if (fstat(fd, &info) != 0) goto done;
    if (!S_ISREG(info.st_mode) || !shared) { errno = ENOTSUP; goto done; }
    canonical = nxio_shared_path(fd);
    if (!canonical) goto done;
    if (nxio_shared_claim(fd, 1) != 0) goto done;
    char translated[FS_MAX_PATH];
    if (fsdevTranslatePath(canonical, NULL, translated) != 0) goto release_claim;
    FsFileSystem independent;
    Result opened = fsOpenSdCardFileSystem(&independent);
    if (R_FAILED(opened)) { errno = EIO; goto release_claim; }
    FsFile contender;
    Result exclusion = fsFsOpenFile(&independent, translated, FsOpenMode_Write, &contender);
    if (R_SUCCEEDED(exclusion)) fsFileClose(&contender);
    fsFsClose(&independent);
    /* TargetLocked was measured for reader/writer, writer/writer and rename /
     * delete through independent filesystem sessions on the actual console.
     * No other error counts as exclusion. If another writer can be opened,
     * refuse to grant instead of substituting a process-local lock table. */
    if (exclusion != 0xe02) { errno = ENOTSUP; goto release_claim; }
    result = 0;
    goto done;
release_claim: {
    int error = errno;
    nxio_shared_claim(fd, 0);
    errno = error;
}
done:
    free(canonical);
    nxio_shared_end(shared); unlock_preserving_errno(lock);
    return result;
}

int nxio_unlock_whole_file(int fd) {
    if (fd < 0 || fd >= NXIO_MAX_DESCRIPTORS) { errno = EBADF; return -1; }
    Mutex *lock = fd_lock(fd); mutexLock(lock);
    int result = -1;
    struct stat info;
    if (fstat(fd, &info) == 0) result = nxio_shared_claim(fd, 0);
    unlock_preserving_errno(lock);
    return result;
}

static int ascii_name(const char *name) {
    for (; *name; ++name) if ((unsigned char)*name >= 128) return 0;
    return 1;
}

char *nxio_canonical_sd_path(const char *path) {
    /* newlib realpath calls lstat for each component; libnx lstat opens data
     * handles and fails while a writer owns the file. Resolve through metadata
     * operations instead, which do not borrow a second data handle. */
    FsFileSystem *filesystem = NULL;
    char *lexical = malloc(FS_MAX_PATH);
    if (!lexical) return NULL;
    if (fsdevTranslatePath(path, &filesystem, lexical) != 0) { free(lexical); return NULL; }
    if (filesystem != fsdevGetDeviceFileSystem("sdmc")) {
        free(lexical); errno = ENOTSUP; return NULL;
    }
    char *components = lexical;
    char *canonical = malloc(PATH_MAX + 1);
    if (!canonical) { free(lexical); return NULL; }
    strcpy(canonical, "sdmc:/");
    char *save = NULL;
    for (char *part = strtok_r(components, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        FsDirEntryType type;
        Result result = fsFsGetEntryType(filesystem, canonical + 5, &type);
        if (R_FAILED(result) || type != FsDirEntryType_Dir) {
            errno = R_FAILED(result) ? (result == 0x202 ? ENOENT : EIO) : ENOTDIR;
            goto error;
        }
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            char *last = strrchr(canonical, '/');
            if (last == canonical + 5) last[1] = 0;
            else if (last > canonical + 5) *last = 0;
            continue;
        }
        char requested[FS_MAX_PATH];
        size_t parent_length = strlen(canonical + 5), part_length = strlen(part);
        if (parent_length + part_length + 2 > sizeof(requested)) { errno = ENAMETOOLONG; goto error; }
        memcpy(requested, canonical + 5, parent_length);
        if (requested[parent_length - 1] != '/') requested[parent_length++] = '/';
        memcpy(requested + parent_length, part, part_length + 1);
        result = fsFsGetEntryType(filesystem, requested, &type);
        if (R_FAILED(result)) {
            /* Match libnx fsdev's published path-error mappings. */
            errno = result == 0x202 ? ENOENT : result == 0x2ee202 ? EINVAL :
                    result == 0x2ee602 ? ENAMETOOLONG : EIO;
            goto error;
        }
        DIR *directory = opendir(canonical);
        if (!directory) goto error;
        char match[NAME_MAX + 1];
        match[0] = 0;
        int matches = 0;
        int ascii = ascii_name(part);
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(part, entry->d_name) == 0) {
                strncpy(match, entry->d_name, NAME_MAX);
                match[NAME_MAX] = 0;
                matches = 1;
                break;
            }
            if (ascii && ascii_name(entry->d_name) && strcasecmp(part, entry->d_name) == 0) {
                strncpy(match, entry->d_name, NAME_MAX);
                match[NAME_MAX] = 0;
                ++matches;
            }
        }
        int error = errno;
        closedir(directory);
        /* libnx's directory device documents ENOENT as end-of-directory. */
        if (entry == NULL && error != 0 && error != ENOENT) { errno = error; goto error; }
        if (matches != 1) { errno = ENOTSUP; goto error; }
        size_t used = strlen(canonical), length = strlen(match);
        if (used + length + 1 > PATH_MAX) { errno = ENAMETOOLONG; goto error; }
        if (canonical[used - 1] != '/') canonical[used++] = '/';
        memcpy(canonical + used, match, length + 1);
    }
    free(lexical);
    return canonical;
error: {
    int error = errno;
    free(lexical);
    free(canonical);
    errno = error;
    return NULL;
    }
}
