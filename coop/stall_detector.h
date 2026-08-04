#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "time/interval.h"

namespace coop
{

class Cooperator;
class Context;

// StallDetector -- a cooperative-scheduling watchdog.
//
// A cooperator is one thread multiplexing many contexts; the whole model rests on every context
// yielding promptly. A context that instead runs a long CPU loop, or slips into a blocking syscall,
// pins the thread and starves every other context sharing it -- the cooperative equivalent of a
// priority inversion, and notoriously hard to catch after the fact because the stall leaves no
// trace once the context finally yields.
//
// This detector catches it live. It watches the cooperator's per-switch state from a sibling thread;
// when one context has held the thread past `threshold` it delivers a signal to the running thread,
// captures that context's stack where it is stuck, and fires a report. The default report logs a
// warning with the context name, the stalled duration, and the captured instruction addresses
// (symbolize with addr2line / the perf symbolizer).
//
// Zero cost when absent. The cooperator's switch seam (Cooperator::StallEnter/StallLeave) is a
// single predicted-not-taken branch that stays dead until a detector arms it. Attached but quiet,
// the cooperator is never perturbed: the watchdog only reads one atomic, and signals the running
// thread solely once a real stall is suspected -- healthy work and idle waiting are never touched.
//
class StallDetector
{
public:
    static constexpr int kMaxFrames = 64;

    struct Report
    {
        // The stalled context. A bare identity token only -- by the time the hook runs the context
        // may have unstuck and been destroyed, so never dereference it; compare it, log it, done.
        //
        Context*    context;
        char        name[64];       // context name, snapshotted inside the capture signal
        int64_t     stalledForUs;   // how long it had held the thread when the stall was reported
        uintptr_t   frames[kMaxFrames];
        int         depth;          // number of captured frames (0 if capture did not land)
    };

    using Hook = std::function<void(const Report&)>;

    // Attach to `co` and report any context that runs longer than `threshold` without yielding.
    // With no hook the default action logs a warning. The detector runs until destroyed.
    //
    StallDetector(Cooperator* co, time::Interval threshold, Hook hook = {});
    ~StallDetector();

    StallDetector(const StallDetector&) = delete;
    StallDetector& operator=(const StallDetector&) = delete;

    // Number of stalls reported so far.
    //
    uint64_t StallCount() const { return m_stallCount.load(std::memory_order_relaxed); }

private:
    void Watch();

    Cooperator*           m_co;
    int64_t               m_thresholdUs;
    Hook                  m_hook;
    std::atomic<bool>     m_stop{false};
    std::atomic<uint64_t> m_stallCount{0};
    std::thread           m_watchdog;
};

} // end namespace coop
