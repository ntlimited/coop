#pragma once

// Performance counter infrastructure for the coop cooperative scheduler.
//
// Three compile-time modes controlled by COOP_PERF_MODE:
//
//   0 (DISABLED):  Macros expand to nothing. Zero overhead, zero storage. Default.
//   1 (ALWAYS_ON): Direct counter increments at every probe site. ~1ns per probe
//                  (single cache-line increment, no atomics needed — cooperator is
//                  single-threaded).
//   2 (DYNAMIC):   NOP/JMP binary patching. When disabled, each probe site is a 2-byte
//                  JMP that skips the increment (predicted branch, ~0 overhead). When
//                  enabled at runtime, the JMP is patched to NOPs and the increment
//                  falls through. Toggle on/off without recompiling.
//

#ifndef COOP_PERF_MODE
#define COOP_PERF_MODE 0
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coop
{
namespace perf
{

enum class Counter : uint32_t
{
    // ---- Scheduler ----
    //
    SchedulerLoop,      // cooperator main loop iterations
    ContextResume,      // context resumes (cooperator -> context switch)
    ContextYield,       // voluntary yields (context -> cooperator switch)
    ContextBlock,       // transitions to BLOCKED state
    ContextSpawn,       // new context spawns
    ContextExit,        // context destructions (stack freed)

    // ---- IO ----
    //
    IoSubmit,           // io_uring SQE submissions (Handle::Submit)
    IoComplete,         // CQE completions (Handle::Complete)
    PollCycle,          // Uring::Poll() invocations
    PollSubmit,         // Uring::Poll() calls that actually submitted SQEs
    PollCqe,            // individual CQEs processed

    // ---- Epoch ----
    //
    EpochAdvance,       // epoch ticks
    EpochPin,           // application-level pins
    EpochUnpin,         // application-level unpins
    DrainCycles,        // reclamation attempts
    DrainReclaimed,     // nodes actually freed

    COUNT
};

// Human-readable counter names indexed by Counter enum.
//
inline const char* CounterName(Counter c)
{
    static const char* s_names[] = {
        // Scheduler
        "scheduler_loop",
        "ctx_resume",
        "ctx_yield",
        "ctx_block",
        "ctx_spawn",
        "ctx_exit",
        // IO
        "io_submit",
        "io_complete",
        "poll_cycle",
        "poll_submit",
        "poll_cqe",
        // Epoch
        "epoch_advance",
        "epoch_pin",
        "epoch_unpin",
        "drain_cycles",
        "drain_reclaimed",
    };
    static_assert(sizeof(s_names) / sizeof(s_names[0]) == static_cast<size_t>(Counter::COUNT));
    auto idx = static_cast<size_t>(c);
    return idx < static_cast<size_t>(Counter::COUNT) ? s_names[idx] : "unknown";
}

// ---- Counter Families ----
//
// Bitmask-based grouping for selective enable/disable in mode 2. Mode 1 ignores families
// entirely (all counters always active). Mode 0 has no counters at all.
//

enum class Family : uint64_t
{
    Scheduler = 1ULL << 0,
    IO        = 1ULL << 1,    Epoch     = 1ULL << 2,

    All       = ~0ULL,
};

inline constexpr Family operator|(Family a, Family b)
{
    return static_cast<Family>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline constexpr Family operator&(Family a, Family b)
{
    return static_cast<Family>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}

inline constexpr Family operator~(Family a)
{
    return static_cast<Family>(~static_cast<uint64_t>(a));
}

inline constexpr bool HasFamily(Family mask, Family f)
{
    return (static_cast<uint64_t>(mask) & static_cast<uint64_t>(f)) != 0;
}

// Derive family from counter ID. Unknown counters map to All (always enabled).
//
inline constexpr Family CounterFamily(Counter c)
{
    switch (c)
    {
        case Counter::SchedulerLoop:
        case Counter::ContextResume:
        case Counter::ContextYield:
        case Counter::ContextBlock:
        case Counter::ContextSpawn:
        case Counter::ContextExit:
            return Family::Scheduler;

        case Counter::IoSubmit:
        case Counter::IoComplete:
        case Counter::PollCycle:
        case Counter::PollSubmit:
        case Counter::PollCqe:
            return Family::IO;

        case Counter::EpochAdvance:
        case Counter::EpochPin:
        case Counter::EpochUnpin:
        case Counter::DrainCycles:
        case Counter::DrainReclaimed:
            return Family::Epoch;

        default:
            return Family::All;
    }
}

// Human-readable family name. Returns nullptr for unknown bits.
//
inline const char* FamilyName(Family f)
{
    switch (f)
    {
        case Family::Scheduler: return "scheduler";
        case Family::IO:        return "io";        case Family::Epoch:     return "epoch";
        default:                return nullptr;
    }
}

// All individual families for iteration.
//
inline constexpr Family s_allFamilies[] = {
    Family::Scheduler,
    Family::IO,    Family::Epoch,
};

inline constexpr size_t kFamilyCount = sizeof(s_allFamilies) / sizeof(s_allFamilies[0]);

#if COOP_PERF_MODE > 0

struct Counters
{
    uint64_t values[static_cast<size_t>(Counter::COUNT)] = {};

    void Inc(Counter id) { values[static_cast<size_t>(id)]++; }

    void IncBy(Counter id, uint64_t n) { values[static_cast<size_t>(id)] += n; }

    uint64_t Get(Counter id) const { return values[static_cast<size_t>(id)]; }

    void Reset() { memset(values, 0, sizeof(values)); }
};

#else

// Empty struct when perf is disabled — zero storage cost.
//
struct Counters {};

#endif

} // end namespace coop::perf
} // end namespace coop
