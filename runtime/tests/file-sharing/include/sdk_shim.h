#ifndef NXIO_TEST_SDK_SHIM_H
#define NXIO_TEST_SDK_SHIM_H
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef pthread_mutex_t Mutex;
typedef pthread_mutex_t RMutex;
static inline void mutexLock(Mutex *lock) { pthread_mutex_lock(lock); }
static inline void mutexUnlock(Mutex *lock) { pthread_mutex_unlock(lock); }
static inline bool mutexTryLock(Mutex *lock) { return pthread_mutex_trylock(lock) == 0; }
static inline void rmutexInit(RMutex *lock) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
static inline void rmutexLock(RMutex *lock) { pthread_mutex_lock(lock); }
static inline void rmutexUnlock(RMutex *lock) { pthread_mutex_unlock(lock); }
typedef uint32_t Result;
typedef uint64_t u64;
typedef uint32_t u32;
typedef int64_t s64;
typedef int32_t s32;
#define R_SUCCEEDED(value) ((value) == 0)
#define R_FAILED(value) ((value) != 0)
#define FS_MAX_PATH 4096
typedef struct { unsigned session; } FsFileSystem;
typedef struct { void *state; } FsFile;
typedef enum { FsDirEntryType_Dir, FsDirEntryType_File } FsDirEntryType;
enum { FsOpenMode_Read = 1, FsOpenMode_Write = 2 };
typedef struct { u64 created, modified, accessed; int is_valid; } FsTimeStampRaw;
typedef struct { int year, month, day, hour, minute, second; } TimeCalendarTime;
int fsdevTranslatePath(const char *, FsFileSystem **, char *);
FsFileSystem *fsdevGetDeviceFileSystem(const char *);
Result fsdevGetLastResult(void);
Result fsFsGetEntryType(FsFileSystem *, const char *, FsDirEntryType *);
Result fsFsGetFileTimeStampRaw(FsFileSystem *, const char *, FsTimeStampRaw *);
Result timeToPosixTimeWithMyRule(const TimeCalendarTime *, u64 *, size_t, s32 *);
Result fsOpenSdCardFileSystem(FsFileSystem *);
Result fsFsOpenFile(FsFileSystem *, const char *, u32, FsFile *);
void fsFileClose(FsFile *);
void fsFsClose(FsFileSystem *);

struct _reent { int _errno; void *deviceData; };
extern _Thread_local struct _reent nx_test_reent;
#define _REENT (&nx_test_reent)
typedef struct { unsigned device, refcount; void *fileStruct; } __handle;
typedef struct {
    const char *name;
    size_t structSize;
    int (*open_r)(struct _reent *, void *, const char *, int, int);
    int (*close_r)(struct _reent *, void *);
    ssize_t (*write_r)(struct _reent *, void *, const char *, size_t);
    ssize_t (*read_r)(struct _reent *, void *, char *, size_t);
    off_t (*seek_r)(struct _reent *, void *, off_t, int);
    int (*fstat_r)(struct _reent *, void *, struct stat *);
    int (*ftruncate_r)(struct _reent *, void *, off_t);
    int (*fsync_r)(struct _reent *, void *);
    void *deviceData;
} devoptab_t;
enum { STD_MAX = 35 };
extern const devoptab_t *devoptab_list[STD_MAX];
int AddDevice(const devoptab_t *);
const devoptab_t *GetDeviceOpTab(const char *);
int __alloc_handle(int);
void __release_handle(int);
__handle *__get_handle(int);

int nx_test_open(const char *, int, ...);
int nx_test_close(int);
ssize_t nx_test_read(int, void *, size_t);
ssize_t nx_test_write(int, const void *, size_t);
off_t nx_test_lseek(int, off_t, int);
int nx_test_fsync(int);
int nx_test_ftruncate(int, off_t);
int nx_test_fstat(int, struct stat *);
int nx_test_stat(const char *, struct stat *);
int nx_test_lstat(const char *, struct stat *);
unsigned nx_test_native_handles(void);
unsigned nx_test_sdk_handles(void);
/* Cumulative backend callback attempts, independent of bridge telemetry.
 * read_bytes counts bytes actually returned by the real fixture-file read. */
struct nx_test_io_stats {
    uint64_t opens, reads, stats, seeks, read_bytes;
};
void nx_test_io_snapshot(struct nx_test_io_stats *out);
void nx_test_fail_upgrade(int fail_restore);
void nx_test_clear_failures(void);
void nx_test_fail_claim_probe(void);
void nx_test_fail_next_stat(void);
void nx_test_fail_next_read(void);
void nx_test_fail_next_seek(void);
void nx_test_shorten_next_read(size_t maximum);
/* Queue bounded real-read outcomes: -1 fails with EIO; nonnegative values cap
 * bytes returned by the next read. A page-sized cap allows a full page fill. */
void nx_test_queue_read_result(ssize_t maximum);
#endif
