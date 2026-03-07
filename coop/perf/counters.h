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
//                  executes. Toggle on/off without recompiling.
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
    SchedulerLoop,      // cooperator main loop iterations
    ContextResume,      // context resumes (cooperator -> context switch)
    ContextYield,       // voluntary yields (context -> cooperator switch)
    ContextBlock,       // transitions to BLOCKED state
    ContextSpawn,       // new context spawns
    ContextExit,        // context destructions (stack freed)
    IoSubmit,           // io_uring SQE submissions (Handle::Submit)
    IoComplete,         // CQE completions (Handle::Complete)
    PollCycle,          // Uring::Poll() invocations
    PollSubmit,         // Uring::Poll() calls that actually submitted SQEs
    PollCqe,            // individual CQEs processed
    ChanSend,           // channel item sent (fast or blocking path)
    ChanSendBlock,      // channel send entered blocking path (channel full)
    ChanRecv,           // channel item received (fast or blocking path)
    ChanRecvBlock,      // channel recv entered blocking path (channel empty)
    PassageRecvFast,    // Passage::Recv phase 1: ring pop succeeded immediately
    PassageRecvYield,   // Passage::Recv phase 2: yield loop iteration
    PassageRecvWait,    // Passage::Recv phase 3: CoordinateWithKill entered
    COUNT
};

// Human-readable counter names indexed by Counter enum.
//
inline const char* CounterName(Counter c)
{
    static const char* s_names[] = {
        "scheduler_loop",
        "ctx_resume",
        "ctx_yield",
        "ctx_block",
        "ctx_spawn",
        "ctx_exit",
        "io_submit",
        "io_complete",
        "poll_cycle",
        "poll_submit",
        "poll_cqe",
        "chan_send",
        "chan_send_block",
        "chan_recv",
        "chan_recv_block",
        "passage_recv_fast",
        "passage_recv_yield",
        "passage_recv_wait",
    };
    static_assert(sizeof(s_names) / sizeof(s_names[0]) == static_cast<size_t>(Counter::COUNT));
    auto idx = static_cast<size_t>(c);
    return idx < static_cast<size_t>(Counter::COUNT) ? s_names[idx] : "unknown";
}

#if COOP_PERF_MODE > 0

struct Counters
{
    uint64_t values[static_cast<size_t>(Counter::COUNT)] = {};

    void Inc(Counter id) { values[static_cast<size_t>(id)]++; }

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
