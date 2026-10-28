#ifndef MINESWITCH_MIMALLOC_PAGE_POOL_H
#define MINESWITCH_MIMALLOC_PAGE_POOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct nxmi_pool_stats {
    size_t capacity, reserved, accessible, peak, largest_free;
    size_t allocations, failures;
    size_t last_failure_size, last_failure_alignment;
};
/* All backing is real and already physically mapped. Permission changes do
 * not release physical pages; only free returns a range to this allocator. */
int nxmi_pool_init(void* base,size_t bytes);
int nxmi_pool_alloc(size_t bytes,size_t alignment,bool commit,void** out);
int nxmi_pool_alloc_some(size_t maximum,size_t minimum,size_t alignment,void** out,size_t* actual);
int nxmi_pool_free(void* base,size_t bytes);
int nxmi_pool_permission(void* base,size_t bytes,bool accessible);
int nxmi_pool_zero(void* base,size_t bytes);
void nxmi_pool_stats(struct nxmi_pool_stats* out,bool scan_largest);
bool nxmi_pool_owns(const void* p,size_t bytes);
void nxmi_pool_dump_trace(void);
#endif
