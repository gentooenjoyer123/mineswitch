#ifndef MINESWITCH_NX_SAFEFETCH_H
#define MINESWITCH_NX_SAFEFETCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return fallback only if the exact load instruction faults. A successfully
 * read value equal to fallback is intentionally indistinguishable, matching
 * HotSpot's SafeFetch contract. */
int32_t nxvm_safe_fetch32(const int32_t *address, int32_t fallback);
intptr_t nxvm_safe_fetch_word(const intptr_t *address, intptr_t fallback);

#ifdef __cplusplus
}
#endif

#endif
