/* Host-only filesystem model. Data, positions, truncate and fsync use real
 * temporary files. The sharing policy models previously observed Horizon
 * reader/writer TargetLocked results; it is not evidence of device execution. */
#include <sdk_shim.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Thread_local struct _reent nx_test_reent;
static _Thread_local Result last_result;
static FsFileSystem sd = {1};
struct native_file { int fd, flags; char path[FS_MAX_PATH + 6]; struct native_file *next; };
static struct native_file *native_files;
static pthread_mutex_t native_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;
static __handle *handles[1024];
static atomic_int fail_upgrade, fail_restore, fail_probe, fail_stat, fail_read, fail_seek;
static atomic_size_t next_read_limit = ATOMIC_VAR_INIT(SIZE_MAX);
static pthread_mutex_t read_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static ssize_t read_outcomes[16];
static unsigned read_outcome_count, read_outcome_index;
static _Atomic uint64_t native_opens, native_reads, native_stats, native_seeks, native_read_bytes;
const devoptab_t *devoptab_list[STD_MAX];

static int error(struct _reent *r, int value, Result result) {
    r->_errno = errno = value; last_result = result; return -1;
}
static int native_open(struct _reent *r, void *state, const char *path, int flags, int mode) {
    atomic_fetch_add_explicit(&native_opens, 1, memory_order_relaxed);
    struct native_file *file = state;
    pthread_mutex_lock(&native_lock);
    if (((flags & O_ACCMODE) != O_RDONLY && fail_upgrade) || ((flags & O_ACCMODE) == O_RDONLY && fail_restore)) {
        pthread_mutex_unlock(&native_lock); return error(r, EIO, 0xe02);
    }
    for (struct native_file *other = native_files; other; other = other->next) {
        if (!strcmp(other->path, path) && ((flags & O_ACCMODE) != O_RDONLY || (other->flags & O_ACCMODE) != O_RDONLY)) {
            pthread_mutex_unlock(&native_lock); return error(r, EIO, 0xe02);
        }
    }
    int fd = open(path, flags, mode);
    if (fd < 0) { int saved = errno; pthread_mutex_unlock(&native_lock); return error(r, saved, 0); }
    file->fd = fd; file->flags = flags;
    snprintf(file->path, sizeof(file->path), "%s", path);
    file->next = native_files; native_files = file;
    last_result = 0;
    pthread_mutex_unlock(&native_lock);
    return 0;
}
static int native_close(struct _reent *r, void *state) {
    struct native_file *file = state;
    pthread_mutex_lock(&native_lock);
    struct native_file **link = &native_files;
    while (*link && *link != file) link = &(*link)->next;
    if (!*link) { pthread_mutex_unlock(&native_lock); return error(r, EBADF, 0); }
    *link = file->next;
    int result = close(file->fd), saved = errno;
    pthread_mutex_unlock(&native_lock);
    return result < 0 ? error(r, saved, 0) : result;
}
static ssize_t native_read(struct _reent *r, void *state, char *data, size_t count) {
    atomic_fetch_add_explicit(&native_reads, 1, memory_order_relaxed);
    if (atomic_exchange_explicit(&fail_read, 0, memory_order_relaxed)) return error(r, EIO, 0x1234);
    size_t limit = atomic_exchange_explicit(&next_read_limit, SIZE_MAX, memory_order_relaxed);
    pthread_mutex_lock(&read_fault_lock);
    int queued = read_outcome_index < read_outcome_count;
    ssize_t outcome = queued ? read_outcomes[read_outcome_index++] : 0;
    if (read_outcome_index == read_outcome_count) read_outcome_count = read_outcome_index = 0;
    pthread_mutex_unlock(&read_fault_lock);
    if (queued && outcome < 0) return error(r, EIO, 0x1234);
    if (queued && (size_t)outcome < limit) limit = (size_t)outcome;
    if (count > limit) count = limit;
    int fd = ((struct native_file*)state)->fd;
    struct stat info;
    off_t position = lseek(fd, 0, SEEK_CUR);
    if (position < 0 || fstat(fd, &info) < 0) return error(r, errno, 0);
    // Measured by FileSharingProbe on Horizon: a retained position beyond a
    // newly truncated file raises an SDK I/O error, unlike host Linux read.
    if (position > info.st_size) return error(r, EIO, 0x1234);
    ssize_t result = read(fd, data, count);
    if (result < 0) error(r, errno, 0);
    else atomic_fetch_add_explicit(&native_read_bytes, (uint64_t)result, memory_order_relaxed);
    return result;
}
static ssize_t native_write(struct _reent *r, void *state, const char *data, size_t count) {
    ssize_t result = write(((struct native_file*)state)->fd, data, count);
    if (result < 0) error(r, errno, 0);
    return result;
}
static off_t native_seek(struct _reent *r, void *state, off_t offset, int origin) {
    atomic_fetch_add_explicit(&native_seeks, 1, memory_order_relaxed);
    if (atomic_exchange_explicit(&fail_seek, 0, memory_order_relaxed)) return error(r, EIO, 0x1234);
    off_t result = lseek(((struct native_file*)state)->fd, offset, origin);
    if (result < 0) error(r, errno, 0);
    return result;
}
static int native_stat(struct _reent *r, void *state, struct stat *info) {
    atomic_fetch_add_explicit(&native_stats, 1, memory_order_relaxed);
    if (atomic_exchange_explicit(&fail_stat, 0, memory_order_relaxed)) return error(r, EIO, 0x1234);
    int result = fstat(((struct native_file*)state)->fd, info);
    return result < 0 ? error(r, errno, 0) : result;
}
static int native_truncate(struct _reent *r, void *state, off_t length) {
    int result = ftruncate(((struct native_file*)state)->fd, length);
    return result < 0 ? error(r, errno, 0) : result;
}
static int native_sync(struct _reent *r, void *state) {
    int result = fsync(((struct native_file*)state)->fd);
    return result < 0 ? error(r, errno, 0) : result;
}
static const devoptab_t native_operations = {
    .name = "sdmc", .structSize = sizeof(struct native_file), .open_r = native_open,
    .close_r = native_close, .write_r = native_write, .read_r = native_read,
    .seek_r = native_seek, .fstat_r = native_stat, .ftruncate_r = native_truncate, .fsync_r = native_sync
};
int AddDevice(const devoptab_t *device) {
    pthread_mutex_lock(&handles_lock);
    for (int i = 4; i < STD_MAX; ++i) {
        if (!devoptab_list[i]) { devoptab_list[i] = device; pthread_mutex_unlock(&handles_lock); return i; }
    }
    pthread_mutex_unlock(&handles_lock); errno = ENFILE; return -1;
}
const devoptab_t *GetDeviceOpTab(const char *path) { (void)path; return &native_operations; }
int __alloc_handle(int device) {
    pthread_mutex_lock(&handles_lock);
    for (int i = 3; i < 1024; ++i) {
        if (!handles[i]) {
            __handle *handle = calloc(1, sizeof(*handle) + devoptab_list[device]->structSize);
            if (!handle) { pthread_mutex_unlock(&handles_lock); errno = ENOMEM; return -1; }
            handle->device = (unsigned)device; handle->refcount = 1; handle->fileStruct = handle + 1;
            handles[i] = handle; pthread_mutex_unlock(&handles_lock); return i;
        }
    }
    pthread_mutex_unlock(&handles_lock); errno = ENFILE; return -1;
}
void __release_handle(int fd) {
    pthread_mutex_lock(&handles_lock);
    free(handles[fd]); handles[fd] = NULL;
    pthread_mutex_unlock(&handles_lock);
}
__handle *__get_handle(int fd) {
    if (fd < 0 || fd >= 1024) return NULL;
    pthread_mutex_lock(&handles_lock);
    __handle *result = handles[fd];
    pthread_mutex_unlock(&handles_lock); return result;
}
int nx_test_open(const char *path, int flags, ...) {
    va_list args; va_start(args, flags); int mode = (flags & O_CREAT) ? va_arg(args, int) : 0; va_end(args);
    pthread_mutex_lock(&handles_lock); devoptab_list[3] = &native_operations; pthread_mutex_unlock(&handles_lock);
    int fd = __alloc_handle(3);
    if (fd < 0) return -1;
    if (native_open(_REENT, __get_handle(fd)->fileStruct, path, flags, mode) < 0) { __release_handle(fd); return -1; }
    return fd;
}
int nx_test_close(int fd) {
    pthread_mutex_lock(&handles_lock);
    __handle *handle = fd >= 0 && fd < 1024 ? handles[fd] : NULL;
    if (handle) handles[fd] = NULL;
    pthread_mutex_unlock(&handles_lock);
    if (!handle) { errno = EBADF; return -1; }
    int result = devoptab_list[handle->device]->close_r(_REENT, handle->fileStruct);
    if (result < 0) errno = _REENT->_errno;
    free(handle); return result;
}
#define CALLBACK(name, type, arguments, call) \
type nx_test_##name arguments { \
    __handle *handle = __get_handle(fd); \
    if (!handle) { errno = EBADF; return -1; } \
    type result = (call); \
    if (result < 0) errno = _REENT->_errno; \
    return result; \
}
CALLBACK(read, ssize_t, (int fd, void *data, size_t size), devoptab_list[handle->device]->read_r(_REENT, handle->fileStruct, data, size))
CALLBACK(write, ssize_t, (int fd, const void *data, size_t size), devoptab_list[handle->device]->write_r(_REENT, handle->fileStruct, data, size))
CALLBACK(lseek, off_t, (int fd, off_t offset, int origin), devoptab_list[handle->device]->seek_r(_REENT, handle->fileStruct, offset, origin))
CALLBACK(fsync, int, (int fd), devoptab_list[handle->device]->fsync_r(_REENT, handle->fileStruct))
CALLBACK(ftruncate, int, (int fd, off_t length), devoptab_list[handle->device]->ftruncate_r(_REENT, handle->fileStruct, length))
CALLBACK(fstat, int, (int fd, struct stat *info), devoptab_list[handle->device]->fstat_r(_REENT, handle->fileStruct, info))

static int path_stat(const char *path, struct stat *info, int follow) {
    pthread_mutex_lock(&native_lock);
    for (struct native_file *file = native_files; file; file = file->next) {
        if (!strcmp(file->path, path) && (file->flags & O_ACCMODE) != O_RDONLY) {
            pthread_mutex_unlock(&native_lock); return error(_REENT, EIO, 0xe02);
        }
    }
    pthread_mutex_unlock(&native_lock);
    int result = follow ? stat(path, info) : lstat(path, info);
    if (result < 0) error(_REENT, errno, 0);
    return result;
}
int nx_test_stat(const char *path, struct stat *info) { return path_stat(path, info, 1); }
int nx_test_lstat(const char *path, struct stat *info) { return path_stat(path, info, 0); }
int fsdevTranslatePath(const char *path, FsFileSystem **filesystem, char *translated) {
    if (strncmp(path, "sdmc:/", 6)) { errno = ENOTSUP; return -1; }
    if (strlen(path + 5) >= FS_MAX_PATH) { errno = ENAMETOOLONG; return -1; }
    if (filesystem) *filesystem = &sd;
    strcpy(translated, path + 5); return 0;
}
FsFileSystem *fsdevGetDeviceFileSystem(const char *name) { return !strcmp(name, "sdmc") ? &sd : NULL; }
Result fsdevGetLastResult(void) { return last_result; }
Result fsFsGetEntryType(FsFileSystem *fs, const char *path, FsDirEntryType *type) {
    (void)fs; char native[FS_MAX_PATH + 6]; snprintf(native, sizeof(native), "sdmc:%s", path);
    struct stat info;
    if (stat(native, &info)) return last_result = errno == ENOENT ? 0x202 : 1;
    *type = S_ISDIR(info.st_mode) ? FsDirEntryType_Dir : FsDirEntryType_File;
    return last_result = 0;
}
Result fsFsGetFileTimeStampRaw(FsFileSystem *fs, const char *path, FsTimeStampRaw *times) {
    (void)fs; (void)path; memset(times, 0, sizeof(*times)); return 1;
}
Result timeToPosixTimeWithMyRule(const TimeCalendarTime *calendar, u64 *output, size_t capacity, s32 *count) {
    (void)calendar; (void)output; (void)capacity; (void)count; return 1;
}
Result fsOpenSdCardFileSystem(FsFileSystem *fs) { fs->session = 2; return 0; }
Result fsFsOpenFile(FsFileSystem *fs, const char *path, u32 mode, FsFile *file) {
    (void)fs;
    if (atomic_exchange_explicit(&fail_probe, 0, memory_order_relaxed)) return 0x1234;
    file->state = calloc(1, sizeof(struct native_file));
    if (!file->state) return 1;
    char native[FS_MAX_PATH + 6]; snprintf(native, sizeof(native), "sdmc:%s", path);
    int result = native_open(_REENT, file->state, native, mode & FsOpenMode_Write ? O_RDWR : O_RDONLY, 0);
    if (result < 0) { free(file->state); file->state = NULL; return last_result ? last_result : 1; }
    return 0;
}
void fsFileClose(FsFile *file) { native_close(_REENT, file->state); free(file->state); file->state = NULL; }
void fsFsClose(FsFileSystem *fs) { (void)fs; }
unsigned nx_test_native_handles(void) {
    unsigned count = 0; pthread_mutex_lock(&native_lock);
    for (struct native_file *file = native_files; file; file = file->next) ++count;
    pthread_mutex_unlock(&native_lock); return count;
}
unsigned nx_test_sdk_handles(void) {
    unsigned count = 0; pthread_mutex_lock(&handles_lock);
    for (unsigned i = 3; i < 1024; ++i) if (handles[i]) ++count;
    pthread_mutex_unlock(&handles_lock); return count;
}
void nx_test_io_snapshot(struct nx_test_io_stats *out) {
    out->opens = atomic_load_explicit(&native_opens, memory_order_relaxed);
    out->reads = atomic_load_explicit(&native_reads, memory_order_relaxed);
    out->stats = atomic_load_explicit(&native_stats, memory_order_relaxed);
    out->seeks = atomic_load_explicit(&native_seeks, memory_order_relaxed);
    out->read_bytes = atomic_load_explicit(&native_read_bytes, memory_order_relaxed);
}
void nx_test_fail_upgrade(int restoration) {
    atomic_store_explicit(&fail_upgrade, 1, memory_order_relaxed);
    atomic_store_explicit(&fail_restore, restoration, memory_order_relaxed);
}
void nx_test_clear_failures(void) {
    atomic_store_explicit(&fail_upgrade, 0, memory_order_relaxed);
    atomic_store_explicit(&fail_restore, 0, memory_order_relaxed);
    atomic_store_explicit(&fail_probe, 0, memory_order_relaxed);
    atomic_store_explicit(&fail_stat, 0, memory_order_relaxed);
    atomic_store_explicit(&fail_read, 0, memory_order_relaxed);
    atomic_store_explicit(&fail_seek, 0, memory_order_relaxed);
    atomic_store_explicit(&next_read_limit, SIZE_MAX, memory_order_relaxed);
    pthread_mutex_lock(&read_fault_lock);
    read_outcome_count = read_outcome_index = 0;
    pthread_mutex_unlock(&read_fault_lock);
}
void nx_test_fail_claim_probe(void) { atomic_store_explicit(&fail_probe, 1, memory_order_relaxed); }
void nx_test_fail_next_stat(void) { atomic_store_explicit(&fail_stat, 1, memory_order_relaxed); }
void nx_test_fail_next_read(void) { atomic_store_explicit(&fail_read, 1, memory_order_relaxed); }
void nx_test_fail_next_seek(void) { atomic_store_explicit(&fail_seek, 1, memory_order_relaxed); }
void nx_test_shorten_next_read(size_t maximum) { atomic_store_explicit(&next_read_limit, maximum, memory_order_relaxed); }
void nx_test_queue_read_result(ssize_t maximum) {
    pthread_mutex_lock(&read_fault_lock);
    if (read_outcome_count == sizeof(read_outcomes) / sizeof(read_outcomes[0])) abort();
    read_outcomes[read_outcome_count++] = maximum;
    pthread_mutex_unlock(&read_fault_lock);
}
