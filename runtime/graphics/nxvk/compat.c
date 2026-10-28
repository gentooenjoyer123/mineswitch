/* Horizon services needed by the static NXVK stack. No dynamic driver loading. */
#include <switch.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#include <stddef.h>

_Static_assert(sizeof(struct pollfd)==8 && offsetof(struct pollfd,events)==4 &&
               offsetof(struct pollfd,revents)==6,"Rust/SDK pollfd ABI must agree");

ssize_t getrandom(void* buffer,size_t bytes,unsigned flags) {
    if (flags & ~3u) { errno=EINVAL;return -1; }
    if (bytes>(size_t)SSIZE_MAX || (!buffer && bytes)) { errno=EINVAL;return -1; }
    randomGet(buffer,bytes);
    return (ssize_t)bytes;
}
#ifndef MINESWITCH_ALLOCATOR_MIMALLOC
int posix_memalign(void** out,size_t alignment,size_t bytes) {
    if (alignment<sizeof(void*) || (alignment&(alignment-1))) return EINVAL;
    int saved_errno=errno;
    void* p=memalign(alignment,bytes);
    int result=p?0:ENOMEM;
    if (p) *out=p;
    errno=saved_errno;
    return result;
}
#endif
/* Homebrew has no POSIX user namespace; SD objects share this identity. */
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
long sysconf(int name) {
    u64 value=0;
    switch (name) {
        case _SC_PAGESIZE:return 4096;
        case _SC_NPROCESSORS_CONF:
        case _SC_NPROCESSORS_ONLN:
            if (R_SUCCEEDED(svcGetInfo(&value,InfoType_CoreMask,CUR_PROCESS_HANDLE,0)) && value)
                return __builtin_popcountll(value);
            break;
        case _SC_PHYS_PAGES:
            if (R_SUCCEEDED(svcGetInfo(&value,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0))) return (long)(value/4096);
            break;
        default:errno=EINVAL;return -1;
    }
    errno=EIO;return -1;
}
/* There are no POSIX signals on Horizon. This does not mask native exceptions. */
int pthread_sigmask(int how,const sigset_t* set,sigset_t* previous) {
    if (set && how!=SIG_BLOCK && how!=SIG_UNBLOCK && how!=SIG_SETMASK) return EINVAL;
    if (previous) memset(previous,0,sizeof(*previous));
    return 0;
}
/* An unsupported lock/cache backend must fail, not report a fictitious lock. */
int flock(int fd,int operation) { (void)fd;(void)operation;errno=ENOTSUP;return -1; }
int dirfd(DIR* directory) { (void)directory;errno=ENOTSUP;return -1; }
int fstatat(int fd,const char* path,struct stat* info,int flags) {
    (void)fd;(void)path;(void)info;(void)flags;errno=ENOTSUP;return -1;
}
int getpwuid_r(uid_t uid,struct passwd* pwd,char* buffer,size_t bytes,struct passwd** result) {
    (void)uid;(void)pwd;(void)buffer;(void)bytes;
    if (result) *result=NULL;
    return ENOTSUP;
}
