#ifndef MINESWITCH_NXVK_GPU_ALIAS_H
#define MINESWITCH_NXVK_GPU_ALIAS_H
#include <stdbool.h>
#include <stddef.h>
/* Cached, unshared, uncompressed GPU backing only. Uncached aliases cannot
 * satisfy Horizon's memory-attribute contract and must use ordinary heap. */
void* mineswitch_nxvk_alloc_cached(size_t bytes,size_t alignment);
bool mineswitch_nxvk_free_cached(void* address);
void mineswitch_nxvk_alias_report(void);
#endif
