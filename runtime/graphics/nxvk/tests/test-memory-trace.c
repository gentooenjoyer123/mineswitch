#include <assert.h>
#include "../memory_trace.c"

static Result create_result,close_result;
Result __real_jitCreate(Jit* jit, size_t bytes) {
    if (!create_result) jit->size=(bytes+4095)&~(size_t)4095;
    return create_result;
}
Result __real_jitClose(Jit* jit) {
    if (!close_result) jit->size=0;
    return close_result;
}

static void* cycle(void* unused) {
    (void)unused;
    for (unsigned i=0;i<10000;i++) {
        mineswitch_nxvk_memory_event(NXVK_BACKING_ALLOC,65536);
        mineswitch_nxvk_memory_event(NXVK_CACHE_PUT,65536);
        mineswitch_nxvk_memory_event(NXVK_CACHE_TAKE,65536);
        mineswitch_nxvk_memory_event(NXVK_BACKING_FREE,65536);
    }
    return NULL;
}

int main(void) {
    assert(pthread_mutex_init(&lock,NULL)==0);
    mineswitch_nxvk_memory_event(NXVK_BACKING_ALLOC,1048576);
    mineswitch_nxvk_memory_event(NXVK_CACHE_PUT,1048576);
    assert(counters.backing==1048576 && counters.cached==1048576);
    mineswitch_nxvk_memory_event(NXVK_CACHE_TAKE,1048576);
    assert(counters.backing==1048576 && counters.cached==0 && counters.hits==1);
    mineswitch_nxvk_memory_event(NXVK_CACHE_PUT,1048576);
    mineswitch_nxvk_memory_event(NXVK_CACHE_EVICT,1048576);
    assert(counters.backing==1048576 && counters.cached==0 && counters.evictions==1);
    mineswitch_nxvk_memory_event(NXVK_BACKING_FREE,1048576);
    mineswitch_nxvk_memory_event(NXVK_CACHE_MISS,0);
    assert(counters.backing==0 && counters.blocks==0 && counters.cached_blocks==0);
    Jit jit={0};
    create_result=7;
    assert(__wrap_jitCreate(&jit,4097)==7 && counters.jit==0);
    create_result=0;
    assert(__wrap_jitCreate(&jit,4097)==0 && counters.jit==8192 && counters.jit_blocks==1);
    close_result=8;
    assert(__wrap_jitClose(&jit)==8 && counters.jit==8192);
    close_result=0;
    assert(__wrap_jitClose(&jit)==0 && counters.jit==0 && counters.jit_blocks==0);
    pthread_t threads[4];
    for (unsigned i=0;i<4;i++) assert(pthread_create(&threads[i],NULL,cycle,NULL)==0);
    for (unsigned i=0;i<4;i++) assert(pthread_join(threads[i],NULL)==0);
    assert(counters.backing==0 && counters.blocks==0 && counters.cached==0);
    assert(counters.cached_blocks==0 && counters.hits==40001 && counters.errors==0);
    mineswitch_nxvk_memory_snapshot();
    puts("NXVK_MEMORY_TRACE_PASS cache reuse/eviction, JIT backing and failure preservation, 40000 concurrent lifecycles");
}
