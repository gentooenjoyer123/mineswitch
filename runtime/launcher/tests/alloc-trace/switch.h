/* Host bookkeeping tests only; this does not emulate Horizon memory APIs. */
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t Result;
typedef struct { size_t size; } Jit;
#define R_SUCCEEDED(value) ((value)==0)
typedef pthread_mutex_t Mutex;
typedef unsigned InfoType;
enum { InfoType_HeapRegionAddress,InfoType_HeapRegionSize,InfoType_AliasRegionAddress,
    InfoType_AliasRegionSize,InfoType_StackRegionAddress,InfoType_StackRegionSize,
    InfoType_AslrRegionAddress,InfoType_AslrRegionSize,InfoType_SystemResourceSizeTotal };
#define CUR_PROCESS_HANDLE 0
static inline void mutexLock(Mutex* p) { pthread_mutex_lock(p); }
static inline void mutexUnlock(Mutex* p) { pthread_mutex_unlock(p); }
static inline bool mutexTryLock(Mutex* p) { return pthread_mutex_trylock(p)==0; }
static inline Result svcGetInfo(u64* out,InfoType t,int p,int n) {
    (void)out;(void)t;(void)p;(void)n;return 1;
}
