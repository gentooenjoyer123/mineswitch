#ifndef MINESWITCH_MIMALLOC_LARGE_PAGES_H
#define MINESWITCH_MIMALLOC_LARGE_PAGES_H
#include <stdbool.h>
#include <stddef.h>
/* True means the large-allocation route handled the request, including OOM.
 * Bookkeeping is outside payload pages, so nxvm/JIT may change permissions. */
bool nxmi_large_alloc(size_t bytes,size_t alignment,void** out);
bool nxmi_large_lookup(const void* p,size_t* requested,size_t* backing);
bool nxmi_large_free(void* p);
bool nxmi_large_shrink(void* p,size_t bytes);
void nxmi_large_report(void);
#endif
