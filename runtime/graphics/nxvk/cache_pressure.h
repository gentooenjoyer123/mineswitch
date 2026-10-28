#ifndef MINESWITCH_NXVK_CACHE_PRESSURE_H
#define MINESWITCH_NXVK_CACHE_PRESSURE_H
#include <stdbool.h>
#include <stddef.h>
/* Callbacks may free only idle backing stores, never live GPU resources.
 * They must try-lock their cache and return immediately if it is busy. */
typedef size_t (*mineswitch_cache_trim_fn)(void*);
bool mineswitch_nxvk_register_cache(void*, mineswitch_cache_trim_fn);
void mineswitch_nxvk_unregister_cache(void*);
size_t mineswitch_nxvk_reclaim_caches(void);
#endif
