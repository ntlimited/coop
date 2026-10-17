#if defined(__aarch64__)

.text

// Seed d8-d15 around an ordinary ABI call, then record what survived. Writing this in assembly
// prevents compiler-generated spills in the test from masking a broken context-switch core.
// Preserve the caller's d8-d15 as every normal AAPCS64 function must.
//
.globl CoopTestWithDRegisters
.type CoopTestWithDRegisters, @function
.align 4
CoopTestWithDRegisters:
    stp x29, x30, [sp, #-96]!
    stp x19, x20, [sp, #16]
    stp d8, d9, [sp, #32]
    stp d10, d11, [sp, #48]
    stp d12, d13, [sp, #64]
    stp d14, d15, [sp, #80]
    mov x19, x1
    mov x20, x2
    ldp d8, d9, [x0, #0]
    ldp d10, d11, [x0, #16]
    ldp d12, d13, [x0, #32]
    ldp d14, d15, [x0, #48]
    mov x0, x3
    blr x20
    stp d8, d9, [x19, #0]
    stp d10, d11, [x19, #16]
    stp d12, d13, [x19, #32]
    stp d14, d15, [x19, #48]
    ldp d8, d9, [sp, #32]
    ldp d10, d11, [sp, #48]
    ldp d12, d13, [sp, #64]
    ldp d14, d15, [sp, #80]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #96
    ret
.size CoopTestWithDRegisters, .-CoopTestWithDRegisters

#endif

.section .note.GNU-stack,"",@progbits
