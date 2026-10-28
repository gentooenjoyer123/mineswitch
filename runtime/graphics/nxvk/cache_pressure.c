#include "cache_pressure.h"
#include <switch.h>
#include <errno.h>

enum { CACHE_LIMIT = 8 };
static struct { void* owner; mineswitch_cache_trim_fn trim; } caches[CACHE_LIMIT];
#ifdef MINESWITCH_PRESSURE_HOST
static Mutex registry_lock=PTHREAD_MUTEX_INITIALIZER;
#else
static Mutex registry_lock;
#endif
static _Thread_local bool reclaiming;

bool mineswitch_nxvk_register_cache(void* owner,mineswitch_cache_trim_fn trim) {
    if (!owner || !trim) return false;
    bool added=false;
    mutexLock(&registry_lock);
    for (unsigned i=0;i<CACHE_LIMIT;i++) if (caches[i].owner==owner) {
        mutexUnlock(&registry_lock);return false;
    }
    for (unsigned i=0;i<CACHE_LIMIT;i++) if (!caches[i].owner) {
        caches[i].owner=owner;caches[i].trim=trim;added=true;break;
    }
    mutexUnlock(&registry_lock);
    return added;
}
void mineswitch_nxvk_unregister_cache(void* owner) {
    // Device destruction calls this before taking any of its own locks.
    // Waiting here keeps each context alive until a current trim has finished.
    mutexLock(&registry_lock);
    for (unsigned i=0;i<CACHE_LIMIT;i++) if (caches[i].owner==owner) {
        caches[i].owner=NULL;caches[i].trim=NULL;break;
    }
    mutexUnlock(&registry_lock);
}
size_t mineswitch_nxvk_reclaim_caches(void) {
    int saved_errno=errno;
    if (reclaiming || !mutexTryLock(&registry_lock)) return 0;
    reclaiming=true;
    size_t released=0;
    for (unsigned i=0;i<CACHE_LIMIT;i++) if (caches[i].owner)
        released+=caches[i].trim(caches[i].owner);
    reclaiming=false;
    mutexUnlock(&registry_lock);
    errno=saved_errno;
    return released;
}
