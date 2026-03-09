#include "context_switch.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__)

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

#elif defined(__aarch64__)

// ContextInit prepares a fresh stack so that ContextSwitch can "resume" into it for the first
// time. The layout mirrors what ContextSwitch would have saved via stp instructions:
//
//   96-byte save area (sp offsets):
//     +0:  x29 (fp)  = 0               ← frame chain terminator
//     +8:  x30 (lr)  = _context_trampoline  ← ret branches here
//     +16: x19       = ctx             ← trampoline reads this as Context*
//     +24: x20       = 0
//     +32: x21       = 0
//     +40: x22       = 0
//     +48: x23       = 0
//     +56: x24       = 0
//     +64: x25       = 0
//     +72: x26       = 0
//     +80: x27       = 0
//     +88: x28       = 0
//
// After ContextSwitch restores registers via ldp and executes `ret`, x19 holds Context*,
// x30 holds _context_trampoline (so ret branches there), and control transfers to
// _context_trampoline which tail-calls CoopContextEntry.
//
void* ContextInit(void* stack_top, coop::Context* ctx)
{
    auto* sp = static_cast<uintptr_t*>(stack_top);

    // Allocate 96 bytes (12 registers × 8 bytes) matching ContextSwitch's save area
    //
    sp -= 12;
    memset(sp, 0, 96);

    sp[0] = 0;                                                  // x29 (fp) — frame terminator
    sp[1] = reinterpret_cast<uintptr_t>(_context_trampoline);   // x30 (lr)
    sp[2] = reinterpret_cast<uintptr_t>(ctx);                   // x19 = Context*

    return static_cast<void*>(sp);
}

#else
#error "Unsupported architecture — ContextInit requires x86-64 or aarch64"
#endif
