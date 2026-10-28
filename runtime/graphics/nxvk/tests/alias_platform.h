#ifndef MINESWITCH_ALIAS_PLATFORM_H
#define MINESWITCH_ALIAS_PLATFORM_H
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t Result;
typedef pthread_mutex_t Mutex;
typedef struct VirtmemReservation VirtmemReservation;
#define R_SUCCEEDED(x) ((x)==0)
#define R_FAILED(x) ((x)!=0)
#define Module_Libnx 1
#define LibnxError_BadInput 2
#define MAKERESULT(m,d) ((m)*512+(d))
static inline void mutexLock(Mutex* m) { pthread_mutex_lock(m); }
static inline void mutexUnlock(Mutex* m) { pthread_mutex_unlock(m); }
void virtmemLock(void);
void virtmemUnlock(void);
void* virtmemFindStack(size_t,size_t);
VirtmemReservation* virtmemAddReservation(void*,size_t);
void virtmemRemoveReservation(VirtmemReservation*);
Result svcMapMemory(void*,void*,size_t);
Result svcUnmapMemory(void*,void*,size_t);
void diagAbortWithResult(Result) __attribute__((noreturn));
/* Host CPU aliases are cache coherent. Real GPU/cache-attribute behavior is
 * validated separately by the native probe, not emulated by this fence. */
static inline void armDCacheFlush(void* p,size_t n) { (void)p;(void)n;__sync_synchronize(); }
#endif
