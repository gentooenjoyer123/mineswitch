/*
 * Safe reads for HotSpot on Horizon AArch64.
 *
 * The kernel routes user exceptions through the NRO entry point. libnx's crt0
 * calls __libnx_exception_entry with x0=exception description and x1 pointing
 * at a ThreadExceptionFrameA64. Registers x0..x8 are saved in that frame;
 * x9..x28 must remain untouched. This handler uses only x0..x4 and never
 * touches the stack, TLS, locks, malloc, or libnx's global exception dump.
 *
 * Recover only a data read abort at one of the two exact instructions below.
 * A fault on any unrelated instruction is returned to the kernel as unhandled.
 * This is not general JVM fault handling or Java stack-overflow recovery.
 */

.arch armv8-a

.section .text.nxvm_safe_fetch32, "ax", %progbits
.p2align 2
.global nxvm_safe_fetch32
.type nxvm_safe_fetch32, %function
nxvm_safe_fetch32:
.Lnxvm_fetch32_load:
    ldr w0, [x0]
.Lnxvm_fetch32_resume:
    ret
.size nxvm_safe_fetch32, .-nxvm_safe_fetch32

.section .text.nxvm_safe_fetch_word, "ax", %progbits
.p2align 2
.global nxvm_safe_fetch_word
.type nxvm_safe_fetch_word, %function
nxvm_safe_fetch_word:
.Lnxvm_fetch_word_load:
    ldr x0, [x0]
.Lnxvm_fetch_word_resume:
    ret
.size nxvm_safe_fetch_word, .-nxvm_safe_fetch_word

#ifndef NXVM_HOTSPOT_AARCH64
.section .text.__libnx_exception_entry, "ax", %progbits
.p2align 2
.global __libnx_exception_entry
.type __libnx_exception_entry, %function
__libnx_exception_entry:
    cbz x1, .Lnxvm_unhandled
    /* ThreadExceptionDesc_Other contains AArch64 data aborts. */
    cmp w0, #0x101
    b.ne .Lnxvm_unhandled

    /* ESR EC=0x24: data abort from lower EL; ISS.WnR must be zero. */
    ldr w2, [x1, #108]
    tbnz w2, #6, .Lnxvm_unhandled
    lsr w2, w2, #26
    cmp w2, #0x24
    b.ne .Lnxvm_unhandled

    ldr x2, [x1, #88]             /* frame.elr_el1 */
    adrp x3, .Lnxvm_fetch32_load
    add x3, x3, :lo12:.Lnxvm_fetch32_load
    cmp x2, x3
    b.eq .Lnxvm_recover32
    adrp x3, .Lnxvm_fetch_word_load
    add x3, x3, :lo12:.Lnxvm_fetch_word_load
    cmp x2, x3
    b.ne .Lnxvm_unhandled
    adrp x3, .Lnxvm_fetch_word_resume
    add x3, x3, :lo12:.Lnxvm_fetch_word_resume
    b .Lnxvm_recover

.Lnxvm_recover32:
    adrp x3, .Lnxvm_fetch32_resume
    add x3, x3, :lo12:.Lnxvm_fetch32_resume
.Lnxvm_recover:
    ldr x2, [x1, #8]              /* original x1 = fallback */
    str x2, [x1, #0]              /* returned x0 = fallback */
    str x3, [x1, #88]             /* skip only the guarded load */
    mov w0, wzr
    svc #0x28                    /* svcReturnFromException(success) */
    b .Lnxvm_unhandled            /* documented noreturn */

.Lnxvm_unhandled:
    mov w0, #0xf801               /* same unhandled result as libnx */
    svc #0x28
    b .Lnxvm_unhandled
.size __libnx_exception_entry, .-__libnx_exception_entry
#endif

.section .note.GNU-stack,"",%progbits
