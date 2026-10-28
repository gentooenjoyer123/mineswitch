/* Exact-fault register preservation test. This is not VM generated code. */
.arch armv8-a
.text
.p2align 2
.global nx_exception_register_probe
.type nx_exception_register_probe,%function
nx_exception_register_probe:
    sub sp, sp, #1088
    add x2, sp, #832
    stp x19, x20, [x2, #0]
    stp x21, x22, [x2, #16]
    stp x23, x24, [x2, #32]
    stp x25, x26, [x2, #48]
    stp x27, x28, [x2, #64]
    stp x29, x30, [x2, #80]
    stp q8, q9, [x2, #96]
    stp q10, q11, [x2, #128]
    stp q12, q13, [x2, #160]
    stp q14, q15, [x2, #192]
    str x1, [x2, #224]
    mrs x3, fpcr
    mrs x4, fpsr
    str x3, [x2, #232]
    str x4, [x2, #240]
    mov x1, #1
    mov x2, #2
    mov x3, #3
    mov x4, #4
    mov x5, #5
    mov x6, #6
    mov x7, #7
    mov x8, #8
    mov x9, #9
    mov x10, #10
    mov x11, #11
    mov x12, #12
    mov x13, #13
    mov x14, #14
    mov x15, #15
    mov x16, #16
    mov x17, #17
    mov x18, #18
    mov x19, #19
    mov x20, #20
    mov x21, #21
    mov x22, #22
    mov x23, #23
    mov x24, #24
    mov x25, #25
    mov x26, #26
    mov x27, #27
    mov x28, #28
    mov x29, #29
    mov x30, #30
    movi v0.16b, #1
    movi v1.16b, #2
    movi v2.16b, #3
    movi v3.16b, #4
    movi v4.16b, #5
    movi v5.16b, #6
    movi v6.16b, #7
    movi v7.16b, #8
    movi v8.16b, #9
    movi v9.16b, #10
    movi v10.16b, #11
    movi v11.16b, #12
    movi v12.16b, #13
    movi v13.16b, #14
    movi v14.16b, #15
    movi v15.16b, #16
    movi v16.16b, #17
    movi v17.16b, #18
    movi v18.16b, #19
    movi v19.16b, #20
    movi v20.16b, #21
    movi v21.16b, #22
    movi v22.16b, #23
    movi v23.16b, #24
    movi v24.16b, #25
    movi v25.16b, #26
    movi v26.16b, #27
    movi v27.16b, #28
    movi v28.16b, #29
    movi v29.16b, #30
    movi v30.16b, #31
    movi v31.16b, #32
    mov x0, #0x00400000
    msr fpcr, x0
    mov x0, #31
    msr fpsr, x0
    mov x0, #0
    cmp wzr, wzr
.global nx_exception_probe_load
nx_exception_probe_load:
    ldr xzr, [x0]
.global nx_exception_probe_resume
nx_exception_probe_resume:
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mov x0, sp
    str x0, [sp, #248]
    mrs x3, fpcr
    mrs x4, fpsr
    stp x3, x4, [sp, #272]
    mrs x3, nzcv
    str w3, [sp, #288]
    stp q0, q1, [sp, #320]
    stp q2, q3, [sp, #352]
    stp q4, q5, [sp, #384]
    stp q6, q7, [sp, #416]
    stp q8, q9, [sp, #448]
    stp q10, q11, [sp, #480]
    stp q12, q13, [sp, #512]
    stp q14, q15, [sp, #544]
    stp q16, q17, [sp, #576]
    stp q18, q19, [sp, #608]
    stp q20, q21, [sp, #640]
    stp q22, q23, [sp, #672]
    stp q24, q25, [sp, #704]
    stp q26, q27, [sp, #736]
    stp q28, q29, [sp, #768]
    stp q30, q31, [sp, #800]
    ldr x0, [sp, #1056]
    mov x1, sp
    mov x2, #832
    bl memcpy
    add x2, sp, #832
    ldr x3, [x2, #232]
    ldr x4, [x2, #240]
    msr fpcr, x3
    msr fpsr, x4
    ldp q8, q9, [x2, #96]
    ldp q10, q11, [x2, #128]
    ldp q12, q13, [x2, #160]
    ldp q14, q15, [x2, #192]
    ldp x19, x20, [x2, #0]
    ldp x21, x22, [x2, #16]
    ldp x23, x24, [x2, #32]
    ldp x25, x26, [x2, #48]
    ldp x27, x28, [x2, #64]
    ldp x29, x30, [x2, #80]
    add sp, sp, #1088
    ret
.size nx_exception_register_probe,.-nx_exception_register_probe

.p2align 2
.global nx_exception_trap_probe
.type nx_exception_trap_probe,%function
nx_exception_trap_probe:
    sub sp, sp, #1088
    add x2, sp, #832
    stp x19, x20, [x2, #0]
    stp x21, x22, [x2, #16]
    stp x23, x24, [x2, #32]
    stp x25, x26, [x2, #48]
    stp x27, x28, [x2, #64]
    stp x29, x30, [x2, #80]
    stp q8, q9, [x2, #96]
    stp q10, q11, [x2, #128]
    stp q12, q13, [x2, #160]
    stp q14, q15, [x2, #192]
    str x1, [x2, #224]
    mrs x3, fpcr
    mrs x4, fpsr
    str x3, [x2, #232]
    str x4, [x2, #240]
    mov x1, #1
    mov x2, #2
    mov x3, #3
    mov x4, #4
    mov x5, #5
    mov x6, #6
    mov x7, #7
    mov x8, #8
    mov x9, #9
    mov x10, #10
    mov x11, #11
    mov x12, #12
    mov x13, #13
    mov x14, #14
    mov x15, #15
    mov x16, #16
    mov x17, #17
    mov x18, #18
    mov x19, #19
    mov x20, #20
    mov x21, #21
    mov x22, #22
    mov x23, #23
    mov x24, #24
    mov x25, #25
    mov x26, #26
    mov x27, #27
    mov x28, #28
    mov x29, #29
    mov x30, #30
    movi v0.16b, #1
    movi v1.16b, #2
    movi v2.16b, #3
    movi v3.16b, #4
    movi v4.16b, #5
    movi v5.16b, #6
    movi v6.16b, #7
    movi v7.16b, #8
    movi v8.16b, #9
    movi v9.16b, #10
    movi v10.16b, #11
    movi v11.16b, #12
    movi v12.16b, #13
    movi v13.16b, #14
    movi v14.16b, #15
    movi v15.16b, #16
    movi v16.16b, #17
    movi v17.16b, #18
    movi v18.16b, #19
    movi v19.16b, #20
    movi v20.16b, #21
    movi v21.16b, #22
    movi v22.16b, #23
    movi v23.16b, #24
    movi v24.16b, #25
    movi v25.16b, #26
    movi v26.16b, #27
    movi v27.16b, #28
    movi v28.16b, #29
    movi v29.16b, #30
    movi v30.16b, #31
    movi v31.16b, #32
    mov x0, #0x00400000
    msr fpcr, x0
    mov x0, #31
    msr fpsr, x0
    mov x0, #0
    cmp wzr, wzr
.global nx_exception_probe_trap
nx_exception_probe_trap:
    .inst 0xd4bbd5a1 /* upstream HotSpot dcps1 #0xdead */
.global nx_exception_probe_trap_resume
nx_exception_probe_trap_resume:
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mov x0, sp
    str x0, [sp, #248]
    mrs x3, fpcr
    mrs x4, fpsr
    stp x3, x4, [sp, #272]
    mrs x3, nzcv
    str w3, [sp, #288]
    stp q0, q1, [sp, #320]
    stp q2, q3, [sp, #352]
    stp q4, q5, [sp, #384]
    stp q6, q7, [sp, #416]
    stp q8, q9, [sp, #448]
    stp q10, q11, [sp, #480]
    stp q12, q13, [sp, #512]
    stp q14, q15, [sp, #544]
    stp q16, q17, [sp, #576]
    stp q18, q19, [sp, #608]
    stp q20, q21, [sp, #640]
    stp q22, q23, [sp, #672]
    stp q24, q25, [sp, #704]
    stp q26, q27, [sp, #736]
    stp q28, q29, [sp, #768]
    stp q30, q31, [sp, #800]
    ldr x0, [sp, #1056]
    mov x1, sp
    mov x2, #832
    bl memcpy
    add x2, sp, #832
    ldr x3, [x2, #232]
    ldr x4, [x2, #240]
    msr fpcr, x3
    msr fpsr, x4
    ldp q8, q9, [x2, #96]
    ldp q10, q11, [x2, #128]
    ldp q12, q13, [x2, #160]
    ldp q14, q15, [x2, #192]
    ldp x19, x20, [x2, #0]
    ldp x21, x22, [x2, #16]
    ldp x23, x24, [x2, #32]
    ldp x25, x26, [x2, #48]
    ldp x27, x28, [x2, #64]
    ldp x29, x30, [x2, #80]
    add sp, sp, #1088
    ret
.size nx_exception_trap_probe,.-nx_exception_trap_probe

.section .note.GNU-stack,"",%progbits
