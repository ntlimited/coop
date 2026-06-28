#pragma once

#include <cstdint>

#include "interval.h"

#include "coop/coordinator.h"
#include "coop/io/handle.h"

namespace coop
{

namespace time {

enum class SleepResult : int8_t
{
    Ok,
    Killed,
    Error
};

// Timer-slack quantization.
//
// coop has no timer wheel: every sleep arms its own IORING_OP_TIMEOUT, which the kernel backs with
// a distinct hrtimer. A population of concurrent sleeps at slightly-different deadlines therefore
// arms a distinct kernel timer per deadline and wakes the cooperator thread once per distinct
// expiry — one io_uring_enter return, one scheduler pass, per wakeup. Most of those wakeups exist
// only because the deadlines failed to coincide by a few microseconds.
//
// Slack collapses them. A non-zero slack rounds the sleep's absolute deadline UP to the next
// multiple of the slack interval, so nearby deadlines land on a shared kernel expiry and the kernel
// fires one timer and delivers a batch of completions instead of a scatter of near-simultaneous
// ones. The cost is bounded, one-sided over-sleep: at most one slack interval, never an early wake.
// This is userspace PR_SET_TIMERSLACK and the hashed timing wheel's natural bucket granularity.
//
// It is opt-in and one-sided by deliberate covenant: slack must never be applied to a deadline that
// guards correctness. IO timeouts (Recv/Send/Connect/Resolve and the linked-timeout IO path) carry
// no slack parameter at all and stay exact — a protocol deadline that is allowed to drift is a bug,
// not an optimization. Slack belongs only where the deadline is a hint: a plain Sleep, an idle
// stealer's recheck park. The default is zero (exact), so existing callers are unchanged.
//

// The Sleeper is what actually does the work of sleeping, Sleep just packages it. Timeouts are
// dispatched via IORING_OP_TIMEOUT through the cooperator's io_uring ring. A non-zero slack rounds
// the absolute deadline up to the next slack boundary (see the covenant above).
//
struct Sleeper
{
    Sleeper(Context* ctx, Interval interval, Interval slack = Interval::zero());

    ~Sleeper();

    Coordinator* GetCoordinator();

    // Returns false if the timeout SQE could not be submitted (ring full).
    //
    bool Submit();

    // Blocks until the timeout fires or the context is killed. Returns true if the sleep completed
    // normally, false if the context was killed. Only call after a successful Submit().
    //
    bool Wait();

    // Submit and wait. Returns Ok if completed, Killed if the context was killed, Error if the
    // timeout could not be submitted.
    //
    SleepResult Sleep();

  private:
    // Round the requested interval up so the resulting absolute deadline lands on a slack boundary.
    // Returns the interval unchanged when slack is zero.
    //
    Interval Quantize(Interval interval) const;

    Coordinator m_coordinator;

    Context*    m_context;
    io::Handle  m_handle;
    Interval    m_interval;
    Interval    m_slack;
};

// Kill-aware sleep. Returns Ok if the sleep completed normally, Killed if the context was killed
// before the interval elapsed, Error if the timeout could not be submitted. A non-zero slack opts
// the deadline into quantization (see the covenant above); the default is exact.
//
SleepResult Sleep(Context* ctx, Interval interval, Interval slack = Interval::zero());

SleepResult Sleep(Interval interval, Interval slack = Interval::zero());

} // end namespace time
} // end namespace coop
