#pragma once

#include "counters.h"

// COOP_PERF_INC(counters, id)
//
// Increment a performance counter. Expands differently based on COOP_PERF_MODE:
//
//   Mode 0 (DISABLED):   Nothing. Zero cost.
//   Mode 1 (ALWAYS_ON):  Direct increment. ~1ns (L1 cache hit).
//   Mode 2 (DYNAMIC):    asm goto with patchable JMP. When probes are disabled (default),
//                         a short JMP skips the increment (~0 cost, predicted forward branch).
//                         When enabled at runtime, the JMP is patched to NOPs and the increment
//                         falls through. Probe sites are recorded in the "coop_perf_sites"
//                         ELF section for the patching engine to find.
//
// Usage:
//   COOP_PERF_INC(m_perf, perf::Counter::ContextResume);
//

#if COOP_PERF_MODE == 0

// Disabled — zero overhead, zero code generation.
//
#define COOP_PERF_INC(counters, id) ((void)0)

#elif COOP_PERF_MODE == 1

// Always-on — direct increment.
//
#define COOP_PERF_INC(counters, id) (counters).Inc(id)

#elif COOP_PERF_MODE == 2

// Dynamic — patchable JMP/NOP site with section-registered probe.
//
// Default state: JMP over the increment (disabled, ~0 cost).
// Enabled state: JMP patched to NOPs, falls through to increment.
//
// The section entry records the probe site address and counter ID so the patching engine
// can find and toggle all sites.
//
// __label__ creates a block-scoped label so multiple COOP_PERF_INC invocations in the same
// function don't collide.
//
#define COOP_PERF_INC(counters, id) do {                                                    \
    __label__ __perf_skip;                                                                  \
    asm goto(                                                                               \
        "1: jmp %l[__perf_skip]\n"                                                          \
        ".pushsection coop_perf_sites, \"aw\"\n"                                            \
        ".balign 16\n"                                                                      \
        ".quad 1b\n"                                                                        \
        ".long %c[cid]\n"                                                                   \
        ".long 0\n"                                                                         \
        ".popsection\n"                                                                     \
        : : [cid] "i" (static_cast<uint32_t>(id)) : : __perf_skip);                        \
    (counters).Inc(id);                                                                     \
    __perf_skip: ;                                                                          \
} while(0)

#else
#error "COOP_PERF_MODE must be 0, 1, or 2"
#endif
