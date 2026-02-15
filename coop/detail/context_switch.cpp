#include "context_switch.h"

#include <cstdint>
#include <cstdlib>

// ContextInit prepares a fresh stack so that ContextSwitch can "resume" into it for the first
// time. The layout mirrors what ContextSwitch would have pushed had the context previously
// called ContextSwitch itself:
//
//   stack_top (16-byte aligned by contract):
//     [abort]                ← safety net return address
//     [_context_trampoline]  ← ContextSwitch's `ret` jumps here
//     [rbp = 0]
//     [rbx = 0]
//     [r12 = ctx]            ← trampoline reads this as Context*
//     [r13 = 0]
//     [r14 = 0]
//     [r15 = 0]              ← returned as initial stack pointer
//
// After ContextSwitch pops the 6 registers and executes `ret`, r12 holds Context*, RSP is at
// 8-mod-16 (correct x86-64 ABI alignment at function entry), and control transfers to
// _context_trampoline which tail-calls CoopContextEntry.
//
void* ContextInit(void* stack_top, coop::Context* ctx)
{
    auto* sp = static_cast<uintptr_t*>(stack_top);

    // Safety net: if the trampoline somehow returns, abort
    //
    *--sp = reinterpret_cast<uintptr_t>(abort);

    // Return address for ContextSwitch's `ret` instruction
    //
    *--sp = reinterpret_cast<uintptr_t>(_context_trampoline);

    // Callee-saved registers (order must match ContextSwitch's pop sequence: r15, r14, r13,
    // r12, rbx, rbp — so we push in reverse: rbp first, r15 last)
    //
    *--sp = 0;                                          // rbp
    *--sp = 0;                                          // rbx
    *--sp = reinterpret_cast<uintptr_t>(ctx);           // r12 = Context*
    *--sp = 0;                                          // r13
    *--sp = 0;                                          // r14
    *--sp = 0;                                          // r15

    return static_cast<void*>(sp);
}
