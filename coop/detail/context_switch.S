#if defined(__x86_64__)

// Hand-rolled context switch core for x86-64 System V ABI.
//
// The core saves the callee-saved GPRs (rbp, rbx, r12-r15) and swaps the stack pointer.
// FP controls and status remain shared by the OS thread; see context_switch.h for the yield
// contract. The register-data contract of an ordinary call holds, and ContextSwitch in
// context_switch.h can be a plain, compiler-visible call. Shaving the save set by delegating
// registers to a call-site inline-asm clobber list requires hiding the call from the compiler,
// which provably breaks its red-zone analysis and blinds whole-program codegen to the switch —
// and measures perf-neutral anyway. See docs/context_switch_core_01.md.
//
// %rbp doubles as the carrier for a new context's Context* on first entry (see ContextInit /
// _context_trampoline).
//

.text

// int _coop_switch_context(void** from_sp, void* to_sp, int result)
//
//   rdi = from_sp  — pointer to location where the current RSP is saved
//   rsi = to_sp    — stack pointer to switch to
//   edx = result   — value delivered to the target's switch call site as its return value
//
// Saves the callee-saved registers and the return address on the current stack, stores RSP into
// *from_sp, loads to_sp as the new RSP, restores the target's saved registers, and returns into
// the target with `result` in eax.
//
.globl _coop_switch_context
.type _coop_switch_context, @function
.align 16
_coop_switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)     // save current stack pointer
    movq  %rsi, %rsp       // switch to target stack
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    movl  %edx, %eax       // deliver result to the resumed call site
    ret
.size _coop_switch_context, .-_coop_switch_context

// _context_trampoline — entry point for newly initialized contexts.
//
// ContextInit sets up the stack so that %rbp = Context* and the core's `ret` lands here. We move
// Context* into rdi (first argument per SysV ABI) and tail-call CoopContextEntry.
//
.globl _context_trampoline
.type _context_trampoline, @function
.align 16
_context_trampoline:
    movq  %rbp, %rdi
    jmp   CoopContextEntry
.size _context_trampoline, .-_context_trampoline

#elif defined(__aarch64__)

// Hand-rolled context switch core for AArch64 (AAPCS64).
//
// Saves the callee-saved register data (x19-x28, x29/fp, x30/lr, d8-d15) and swaps the stack
// pointer, invoked as a plain call from ContextSwitch.
// AAPCS64 preserves only the low 64 bits of v8-v15; callers spill any live upper halves.
// FPCR and FPSR remain shared by the OS thread; see context_switch.h for the yield contract.
//

.text

// int _coop_switch_context(void** from_sp, void* to_sp, int result)
//
//   x0 = from_sp  — pointer to location where current SP is saved
//   x1 = to_sp    — stack pointer to switch to
//   w2 = result   — value returned to the target's switch call site
//
.globl _coop_switch_context
.type _coop_switch_context, @function
.align 4
_coop_switch_context:
    // Save 96 bytes of GPRs and 64 bytes of SIMD registers.
    //
    stp x29, x30, [sp, #-160]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]

    mov x3, sp
    str x3, [x0]               // save current stack pointer

    mov sp, x1                  // switch to target stack

    // Restore callee-saved registers
    //
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #160

    mov w0, w2                  // return result
    ret
.size _coop_switch_context, .-_coop_switch_context

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
