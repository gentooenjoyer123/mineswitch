.arch armv8-a
.text
.p2align 2
.global nxvm_stack_test_store
.type nxvm_stack_test_store,%function
nxvm_stack_test_store:
.global nxvm_stack_test_store_pc
nxvm_stack_test_store_pc:
    str w1, [x0]
.global nxvm_stack_test_store_resume
nxvm_stack_test_store_resume:
    ret
.size nxvm_stack_test_store,.-nxvm_stack_test_store
.section .note.GNU-stack,"",%progbits
