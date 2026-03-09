#if defined(__x86_64__)

// Hand-rolled context switch for x86-64 System V ABI.
//
// Replaces setjmp/longjmp and ucontext with a minimal register save/restore that only touches
// callee-saved registers (rbp, rbx, r12-r15) and swaps the stack pointer.
//

.text

// int ContextSwitch(void** from_sp, void* to_sp, int result)
//
//   rdi = from_sp  — pointer to location where current RSP is saved
//   rsi = to_sp    — stack pointer to switch to
//   edx = result   — value returned to the target's ContextSwitch call site
//
// Saves callee-saved registers on the current stack, stores RSP into *from_sp, loads to_sp as the
// new RSP, restores callee-saved registers, and returns `result` to the resumed call site.
//
.globl ContextSwitch
.type ContextSwitch, @function
.align 16
ContextSwitch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq  %rsp, (%rdi)     // save current stack pointer
    movq  %rsi, %rsp        // switch to target stack

    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp

    movl  %edx, %eax        // return result
    ret
.size ContextSwitch, .-ContextSwitch

// _context_trampoline — entry point for newly initialized contexts.
//
// ContextInit sets up the stack so that r12 = Context* and the return address points here.
// We move Context* into rdi (first argument per SysV ABI) and tail-call CoopContextEntry.
//
.globl _context_trampoline
.type _context_trampoline, @function
.align 16
_context_trampoline:
    movq  %r12, %rdi
    jmp   CoopContextEntry
.size _context_trampoline, .-_context_trampoline

#elif defined(__aarch64__)

// Hand-rolled context switch for AArch64 (AAPCS64).
//
// Saves callee-saved registers (x19-x28, x29/fp, x30/lr) and swaps the stack pointer.
// d8-d15 (callee-saved SIMD) are not saved — coop contexts don't use SIMD in the framework.
//

.text

// int ContextSwitch(void** from_sp, void* to_sp, int result)
//
//   x0 = from_sp  — pointer to location where current SP is saved
//   x1 = to_sp    — stack pointer to switch to
//   w2 = result   — value returned to the target's ContextSwitch call site
//
// Saves callee-saved registers on the current stack, stores SP into *from_sp, loads to_sp as the
// new SP, restores callee-saved registers, and returns `result` to the resumed call site.
//
.globl ContextSwitch
.type ContextSwitch, @function
.align 4
ContextSwitch:
    // Save callee-saved registers (96 bytes = 12 registers × 8 bytes)
    //
    stp x29, x30, [sp, #-96]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]

    mov x3, sp
    str x3, [x0]               // save current stack pointer

    mov sp, x1                  // switch to target stack

    // Restore callee-saved registers
    //
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #96

    mov w0, w2                  // return result
    ret
.size ContextSwitch, .-ContextSwitch

// _context_trampoline — entry point for newly initialized contexts.
//
// ContextInit sets up the stack so that x19 = Context* and x30 (lr) points here.
// We move Context* into x0 (first argument per AAPCS64) and tail-call CoopContextEntry.
//
.globl _context_trampoline
.type _context_trampoline, @function
.align 4
_context_trampoline:
    mov x0, x19
    b   CoopContextEntry
.size _context_trampoline, .-_context_trampoline

#else
#error "Unsupported architecture — context_switch.S requires x86-64 or aarch64"
#endif

.section .note.GNU-stack,"",@progbits
