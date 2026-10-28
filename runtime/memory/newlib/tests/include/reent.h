#ifndef MINESWITCH_TEST_NEWLIB_REENT_H
#define MINESWITCH_TEST_NEWLIB_REENT_H
#include <stddef.h>
/* These variants touch only Newlib's first _reent field, _errno, and pass the
 * pointer to malloc locks and _sbrk_r. No other _reent ABI is modeled here. */
struct _reent { int _errno; };
#define _REENT_ERRNO(r) ((r)->_errno)
void *_sbrk_r(struct _reent *, ptrdiff_t);
void __malloc_lock(struct _reent *);
void __malloc_unlock(struct _reent *);
#endif
