/* Build the pinned upstream allocator as one translation unit, without its
 * libc symbol override. Newlib's complete reentrant adapter owns that ABI. */
#include "../../../.upstream/mimalloc/src/static.c"
