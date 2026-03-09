#pragma once

#include <cstddef>

namespace coop
{

struct Context;

} // end namespace coop

// Assembly-level context switch primitive. Saves callee-saved registers on the current stack,
// stores the stack pointer to *from_sp, loads to_sp as the new stack pointer, restores
// registers, and returns `result` to the resumed call site.
//
// x86-64: saves rbp, rbx, r12-r15; swaps RSP.
// aarch64: saves x19-x28, x29 (fp), x30 (lr); swaps SP.
//
extern "C" int ContextSwitch(void** from_sp, void* to_sp, int result);

// Assembly trampoline — first code a new context executes after ContextSwitch restores its
// initial frame. Reads Context* from a callee-saved register (x86-64: r12, aarch64: x19)
// and tail-calls CoopContextEntry.
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
