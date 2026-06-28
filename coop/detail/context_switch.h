#pragma once

#include <cstddef>

namespace coop
{

struct Context;

} // end namespace coop

// Assembly-level context switch core. Saves the minimal register state on the current stack,
// stores the stack pointer to *from_sp, loads to_sp as the new stack pointer, restores the
// minimal state, and returns `result` to the resumed call site.
//
// x86-64: saves only %rbp; swaps RSP. The other callee-saved registers (rbx, r12-r15) are
//         preserved by the compiler at the call site via the ContextSwitch wrapper's clobber
//         list, so only the live ones are spilled per switch.
// aarch64: saves all callee-saved registers (x19-x28, fp, lr); swaps SP.
//
extern "C" int _coop_switch_context(void** from_sp, void* to_sp, int result);

// ContextSwitch — the call-site wrapper around _coop_switch_context.
//
// On x86-64 this is inline asm rather than a plain call: the callee-saved registers the core does
// not save are listed in the clobber list, so the compiler spills and reloads only those live
// across this particular switch. A normal function call would instead force the core to preserve
// all of them unconditionally (the ABI's callee-saved contract), which is the cost this avoids.
//
// On aarch64 (and any other target) the core preserves every callee-saved register itself, so a
// plain call is both correct and complete.
//
#if defined(__x86_64__)

static inline int ContextSwitch(void** from_sp, void* to_sp, int result)
{
    // Pin the arguments to the core's expected registers and mark every register the core may
    // leave indeterminate. rdi/rsi/rdx are in/out so the compiler treats them as clobbered across
    // the switch; rbx and r12-r15 in the clobber list make the compiler spill only the live ones.
    // %rbp is preserved by the core and deliberately not clobbered (it is the frame pointer under
    // -fno-omit-frame-pointer). "memory"/"cc" make the switch a full compiler barrier, matching
    // the old extern-function call boundary.
    //
    register void** from asm("rdi") = from_sp;
    register void*  to   asm("rsi") = to_sp;
    register int    res  asm("edx") = result;
    register int    ret  asm("eax");

    asm volatile(
        "call _coop_switch_context\n"
        : "=r"(ret), "+r"(from), "+r"(to), "+r"(res)
        :
        : "rbx", "rcx", "r8", "r9", "r10", "r11",
          "r12", "r13", "r14", "r15", "memory", "cc");

    return ret;
}

#else

static inline int ContextSwitch(void** from_sp, void* to_sp, int result)
{
    return _coop_switch_context(from_sp, to_sp, result);
}

#endif

// Assembly trampoline — first code a new context executes after the core restores its initial
// frame. Reads Context* from a register set up by ContextInit (x86-64: %rbp, aarch64: x19) and
// tail-calls CoopContextEntry.
//
extern "C" void _context_trampoline();

// C++ wrapper called by _context_trampoline. Invokes the context's entry function and handles
// exit back to the cooperator.
//
extern "C" void CoopContextEntry(coop::Context* ctx);

// Prepares a stack for first entry via ContextSwitch. Returns the initial stack pointer that
// ContextSwitch should switch to.
//
void* ContextInit(void* stack_top, coop::Context* ctx);
