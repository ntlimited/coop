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

.section .note.GNU-stack,"",@progbits
