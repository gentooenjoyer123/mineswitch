/* Diagnostic counters only: no allocation, trimming, or graphics policy. */
#include "memory_trace.h"
#include <switch.h>
#include <inttypes.h>
#include <stdio.h>

typedef struct {
    uint64_t backing, peak, blocks, cached, cached_blocks;
    uint64_t hits, misses, evictions, jit, jit_peak, jit_blocks, errors;
} MemoryTrace;
static MemoryTrace counters;
static Mutex lock;

static void subtract(uint64_t* value, uint64_t amount) {
    if (*value < amount) { counters.errors++; return; }
    *value -= amount;
}

void mineswitch_nxvk_memory_event(unsigned event, uint64_t bytes) {
    mutexLock(&lock);
    switch (event) {
    case NXVK_BACKING_ALLOC:
        counters.backing += bytes;
        counters.blocks++;
        if (counters.backing > counters.peak) counters.peak = counters.backing;
        break;
    case NXVK_BACKING_FREE:
        subtract(&counters.backing, bytes);
        subtract(&counters.blocks, 1);
        break;
    case NXVK_CACHE_PUT:
        counters.cached += bytes;
        counters.cached_blocks++;
        break;
    case NXVK_CACHE_TAKE:
    case NXVK_CACHE_EVICT:
        subtract(&counters.cached, bytes);
        subtract(&counters.cached_blocks, 1);
        if (event == NXVK_CACHE_TAKE) counters.hits++;
        else counters.evictions++;
        break;
    case NXVK_CACHE_MISS: counters.misses++; break;
    default: counters.errors++; break;
    }
    mutexUnlock(&lock);
}

Result __real_jitCreate(Jit*, size_t);
Result __real_jitClose(Jit*);
Result __wrap_jitCreate(Jit* jit, size_t bytes) {
    Result result = __real_jitCreate(jit, bytes);
    if (R_SUCCEEDED(result)) {
        mutexLock(&lock);
        // The RW/RX aliases refer to the same source storage.
        counters.jit += jit->size;
        counters.jit_blocks++;
        if (counters.jit > counters.jit_peak) counters.jit_peak = counters.jit;
        mutexUnlock(&lock);
    }
    return result;
}
Result __wrap_jitClose(Jit* jit) {
    size_t bytes = jit->size;
    Result result = __real_jitClose(jit);
    if (R_SUCCEEDED(result)) {
        mutexLock(&lock);
        subtract(&counters.jit, bytes);
        subtract(&counters.jit_blocks, 1);
        mutexUnlock(&lock);
    }
    return result;
}

void mineswitch_nxvk_memory_snapshot(void) {
    mutexLock(&lock);
    MemoryTrace value = counters;
    mutexUnlock(&lock);
    printf("MINESWITCH_NXVK_MEMORY backing=%" PRIu64 " peak=%" PRIu64
           " blocks=%" PRIu64 " cached=%" PRIu64 " cached_blocks=%" PRIu64
           " cache_hits=%" PRIu64 " cache_misses=%" PRIu64 " evictions=%" PRIu64
           " jit_backing=%" PRIu64 " jit_peak=%" PRIu64 " jit_blocks=%" PRIu64
           " counter_errors=%" PRIu64 "\n",
           value.backing,value.peak,value.blocks,value.cached,value.cached_blocks,
           value.hits,value.misses,value.evictions,value.jit,value.jit_peak,
           value.jit_blocks,value.errors);
}
