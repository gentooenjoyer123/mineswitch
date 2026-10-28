#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "../cache_pressure.c"

static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool entered,finish;
static atomic_bool removed;
static int context;
static size_t held_trim(void* owner) {
    assert(owner==&context);
    assert(mineswitch_nxvk_reclaim_caches()==0);
    pthread_mutex_lock(&gate);
    entered=true;pthread_cond_broadcast(&condition);
    while (!finish) pthread_cond_wait(&condition,&gate);
    pthread_mutex_unlock(&gate);
    return 4096;
}
static void* reclaim_worker(void* unused) {
    (void)unused;assert(mineswitch_nxvk_reclaim_caches()==4096);return NULL;
}
static void* remove_worker(void* unused) {
    (void)unused;mineswitch_nxvk_unregister_cache(&context);atomic_store(&removed,true);return NULL;
}
int main(void) {
    assert(!mineswitch_nxvk_register_cache(NULL,held_trim));
    assert(mineswitch_nxvk_register_cache(&context,held_trim));
    pthread_t reclaim,remove;
    assert(!pthread_create(&reclaim,NULL,reclaim_worker,NULL));
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&condition,&gate);
    pthread_mutex_unlock(&gate);
    assert(mineswitch_nxvk_reclaim_caches()==0); // Busy registry never waits.
    assert(!pthread_create(&remove,NULL,remove_worker,NULL));
    assert(!atomic_load(&removed));
    pthread_mutex_lock(&gate);finish=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
    assert(!pthread_join(reclaim,NULL) && !pthread_join(remove,NULL));
    assert(atomic_load(&removed) && mineswitch_nxvk_reclaim_caches()==0);
    puts("NXVK_CACHE_PRESSURE_PASS nonblocking contention, recursive skip, unregister lifetime synchronization");
}
