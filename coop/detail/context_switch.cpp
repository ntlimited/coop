#include "context_switch.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__)

// ContextInit prepares a fresh stack so that the switch core can "resume" into it for the first
// time. The layout mirrors the minimal state the core leaves on a stack it switched away from:
// only %rbp and a return address (the core saves nothing else — see context_switch.S).
//
//   stack_top (16-byte aligned by contract):
//     [abort]                ← safety net: CoopContextEntry's notional return address
//     [_context_trampoline]  ← the core's `ret` lands here
//     [rbp = ctx]            ← popped into %rbp; trampoline reads it as Context*
//                              (init_sp points here)
//
// After the core executes `popq %rbp` and `ret`, %rbp holds Context*, RSP is at 8-mod-16
// (correct x86-64 ABI alignment at function entry — stack_top is 16-aligned and three 8-byte
// slots leave RSP at stack_top-8 by the time CoopContextEntry runs), and control transfers to
// _context_trampoline, which moves %rbp into rdi and tail-calls CoopContextEntry.
//
void* ContextInit(void* stack_top, coop::Context* ctx)
{
    auto* sp = static_cast<uintptr_t*>(stack_top);

    // Safety net: if CoopContextEntry somehow returns, abort
    //
    *--sp = reinterpret_cast<uintptr_t>(abort);

    // Return address for the core's `ret` instruction
    //
    *--sp = reinterpret_cast<uintptr_t>(_context_trampoline);

    // The only saved register the core restores: %rbp, carrying Context* into the trampoline
    //
    *--sp = reinterpret_cast<uintptr_t>(ctx);           // rbp = Context*

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
