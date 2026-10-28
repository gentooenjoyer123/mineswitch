#ifndef MINESWITCH_NXVK_MEMORY_TRACE_H
#define MINESWITCH_NXVK_MEMORY_TRACE_H
#include <stdint.h>
/* Matches the optional observer in nvkmd_nvgpu_mem.c. Backing is counted
 * once; GPU mappings and transfers into/out of the reuse cache add no bytes. */
enum {
    NXVK_BACKING_ALLOC = 1, NXVK_BACKING_FREE,
    NXVK_CACHE_PUT, NXVK_CACHE_TAKE, NXVK_CACHE_EVICT, NXVK_CACHE_MISS
};
void mineswitch_nxvk_memory_event(unsigned event, uint64_t bytes);
void mineswitch_nxvk_memory_snapshot(void);
#endif
