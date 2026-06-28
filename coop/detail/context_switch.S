#if defined(__x86_64__)

// Hand-rolled context switch core for x86-64 System V ABI.
//
// The core saves only %rbp and swaps the stack pointer. The remaining callee-saved registers
// (rbx, r12-r15) are not touched here -- their preservation is delegated to the compiler at the
// call site (see ContextSwitch in context_switch.h), which lists them in the inline-asm clobber
// list. The compiler then spills and reloads only the ones actually live across a given switch,
// instead of this routine unconditionally saving all six on every switch. On the common yield
// path few callee-saved registers are live, so most switches save nothing beyond %rbp and the
// return address.
//
// %rbp is special: coop builds with -fno-omit-frame-pointer, so the compiler uses it as a frame
// pointer and assumes it survives the call. The core therefore preserves it directly rather than
// clobber-delegating it. It also doubles as the carrier for a new context's Context* on first
// entry (see ContextInit / _context_trampoline).
//

.text

// int _coop_switch_context(void** from_sp, void* to_sp, int result)
//
//   rdi = from_sp  — pointer to location where the current RSP is saved
//   rsi = to_sp    — stack pointer to switch to
//   edx = result   — value delivered to the target's switch call site as its return value
//
// Saves %rbp and the return address on the current stack, stores RSP into *from_sp, loads to_sp
// as the new RSP, restores the target's %rbp, and returns into the target with `result` in eax.
//
.globl _coop_switch_context
.type _coop_switch_context, @function
.align 16
_coop_switch_context:
    pushq %rbp
    movq  %rsp, (%rdi)     // save current stack pointer
    movq  %rsi, %rsp       // switch to target stack
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
// TODO(x86-64-parity): aarch64 still saves every callee-saved register unconditionally. The
// clobber-delegated approach the x86-64 path uses (save only fp, list the rest in the call-site
// clobber list so the compiler spills only live ones) applies here too, but writing and
// validating that asm requires an aarch64 host. The host this lands on is x86-64, so the
// aarch64 core stays the known-correct all-callee-saved version; optimizing it is a documented
// follow-up. The call-site wrapper (context_switch.h) calls this core as a plain C function on
// aarch64, which is correct precisely because the core preserves all callee-saved registers.
//
// Saves callee-saved registers (x19-x28, x29/fp, x30/lr) and swaps the stack pointer.
// d8-d15 (callee-saved SIMD) are not saved — coop contexts don't use SIMD in the framework.
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
