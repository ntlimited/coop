#pragma once

#include <cstddef>

namespace coop
{

struct Context;

} // end namespace coop

// Assembly-level context switch core. Saves the callee-saved register state on the current
// stack, stores the stack pointer to *from_sp, loads to_sp as the new stack pointer, restores
// the target's state, and returns `result` to the resumed call site.
//
// x86-64:  saves %rbp, %rbx, %r12-%r15; swaps RSP.
// aarch64: saves x19-x28, fp, lr; swaps SP.
//
extern "C" int _coop_switch_context(void** from_sp, void* to_sp, int result);

// ContextSwitch — the call-site wrapper around _coop_switch_context.
//
// This is a plain, compiler-visible function call on every architecture, and that is a
// load-bearing design decision. The core saves every callee-saved register, so the ABI
// contract of a normal call holds and the compiler models the switch like any other call:
// it never spills live values into the red zone across it, LTO's whole-program codegen sees a
// real control transfer instead of an opaque asm block, and call-modeling toolchain features
// (sanitizers, CET, PGO) observe a well-formed call.
//
// The tempting alternative — hide the call inside inline asm and delegate the callee-saved
// registers to its clobber list, so the compiler spills only the ones live across a given
// switch — measures perf-neutral on this codebase and is unsafe precisely because it hides
// the call. See docs/context_switch_core_01.md before reintroducing it.
//
static inline int ContextSwitch(void** from_sp, void* to_sp, int result)
{
    return _coop_switch_context(from_sp, to_sp, result);
}

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
