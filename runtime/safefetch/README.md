# Horizon SafeFetch

HotSpot uses SafeFetch to inspect an address which may be unreadable. This
backend executes the read and recovers a fault at that exact instruction; it
does not rely on a prior memory query, which would race against other threads
changing mappings.

`nx_safefetch.S` provides two leaf functions and a strong implementation of
libnx's weak `__libnx_exception_entry`. On a data-read abort at one of the
guarded loads it edits the kernel-supplied `ThreadExceptionFrameA64`: the saved
return value becomes the caller's fallback value and the saved PC advances to
the function's `ret`. `svcReturnFromException(0)` restores execution. Only
registers saved by the kernel in that frame are used as scratch registers.
There is no shared exception buffer, exception stack, TLS state or lock in this
path. Other exceptions are returned to the kernel as unhandled.

The implementation follows the installed libnx public exception-frame layout
and its runtime exception entry convention, audited against libnx source
commit `feebd026ca0f5dcc2119f46ad8e0d16ad3dd4973`,
`nx/source/runtime/exception.s`. `frame_layout.c` checks the offsets and enum
value at compile time. The final ELF must contain a **strong**
`__libnx_exception_entry`, and the load/handler instructions must remain in the
same native image.

This only implements SafeFetch. It does not turn native faults into Java
exceptions, recover arbitrary stack overflows or implement a POSIX signal API.

Build the independent device test with:

```sh
make -C runtime/safefetch/probe
```

The probe checks valid and null reads, reads from an explicitly inaccessible
page, permission restoration, then repeated faults from two native threads.
It queries the process's allowed core mask and assigns distinct permitted
cores when available. It emits `MINESWITCH_SAFEFETCH_*` markers over NXLink and
returns to the loader automatically when all worker resources are closed.
Device execution is required to establish runtime correctness; compilation
alone only verifies assembly and linking.

The OLED device run on 6 October 2026 passed **23 checks with 0 failures**,
including 8,000 guarded faults across permitted cores 0 and 1, and returned to
Sphaira normally. The permitted core mask was `0x7`. Evidence:
[`../../logs/mineswitch-safefetch-probe-20261005T235754Z.log`](../../logs/mineswitch-safefetch-probe-20261005T235754Z.log).
The tested NRO SHA256 is
`3938d364863d3b69181d57434839dede50aac039ffd03c4199375c8f40d8d362`.
This confirms the tested SafeFetch paths, not arbitrary native-fault recovery.
