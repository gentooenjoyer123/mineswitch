/* Keep the exception ABI offsets used by nx_safefetch.S checked against libnx. */
#include <stddef.h>
#include <switch/arm/thread_context.h>

_Static_assert(sizeof(((ThreadExceptionFrameA64 *)0)->cpu_gprs[0]) == 8,
               "Horizon AArch64 register size changed");
_Static_assert(offsetof(ThreadExceptionFrameA64, cpu_gprs) == 0,
               "Horizon AArch64 saved x0 offset changed");
_Static_assert(offsetof(ThreadExceptionFrameA64, elr_el1) == 88,
               "Horizon AArch64 saved PC offset changed");
_Static_assert(offsetof(ThreadExceptionFrameA64, esr) == 108,
               "Horizon AArch64 ESR offset changed");
_Static_assert(ThreadExceptionDesc_Other == 0x101,
               "Horizon data-abort exception description changed");
