#pragma once

#include <cstdint>

#include "interval.h"
#include "timer_queue.h"

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
// A sleep registers its deadline in the owning cooperator's per-thread timer queue rather than
// arming its own kernel timer (see docs/timer_wheel_001.md). The cooperator keeps at most one
// IORING_OP_TIMEOUT in flight, armed for the nearest deadline, and a single expiry services every
// sleep that has come due. Slack composes on top: a non-zero slack rounds the sleep's absolute
// deadline UP to the next multiple of the slack interval, so nearby deadlines collapse onto a shared
// grid point before they enter the queue. The cost is bounded, one-sided over-sleep: at most one
// slack interval, never an early wake. This is userspace PR_SET_TIMERSLACK and the hashed timing
// wheel's natural bucket granularity.
//
// It is opt-in and one-sided by deliberate covenant: slack must never be applied to a deadline that
// guards correctness. IO timeouts (Recv/Send/Connect/Resolve and the linked-timeout IO path) carry
// no slack parameter at all and stay exact — a protocol deadline that is allowed to drift is a bug,
// not an optimization. Slack belongs only where the deadline is a hint: a plain Sleep, an idle
// stealer's recheck park. The default is zero (exact), so existing callers are unchanged.
//

// The Sleeper is what actually does the work of sleeping; Sleep just packages it. The owning
// cooperator's TimerMode picks the backing, transparently to the caller:
//
//   - KernelPerTimer (default): arm an IORING_OP_TIMEOUT through m_handle, exactly as before.
//   - UserspaceQueue: register m_node in the cooperator's timer queue; the queue Releases
//     m_coordinator when the deadline expires.
//
// Both back the same Coordinator the caller blocks on, so Wait()/Sleep() are mode-agnostic. The
// destructor cleans up whichever was used (an unsubmitted Handle and an unlinked node are both
// no-op teardowns). A non-zero slack rounds the absolute deadline up to the next slack boundary (see
// the covenant above).
//
struct Sleeper
{
    Sleeper(Context* ctx, Interval interval, Interval slack = Interval::zero());

    ~Sleeper();

    Coordinator* GetCoordinator();

    // Arm the sleep: acquire the coordinator (so the subsequent Wait blocks) and register the
    // deadline with whichever backing the cooperator's TimerMode selects. Returns false only if the
    // kernel-per-timer path could not submit its SQE (ring full); the queue path always succeeds.
    //
    bool Arm();

    // Blocks until the deadline is serviced or the context is killed. Returns true if the sleep
    // completed normally, false if the context was killed. Only call after a successful Arm().
    //
    bool Wait();

    // Arm and wait. Returns Ok if completed, Killed if the context was killed, Error if the
    // kernel-per-timer SQE could not be submitted.
    //
    SleepResult Sleep();

  private:
    // Absolute monotonic-microsecond deadline for this sleep (queue path), rounded up to a slack
    // boundary when slack is non-zero. Reads the clock once.
    //
    int64_t DeadlineUs() const;

    // Relative interval handed to the kernel timeout (kernel-per-timer path): the requested interval,
    // or -- with slack -- enough extra that the absolute deadline lands on a slack boundary.
    //
    Interval QuantizedInterval() const;

    Coordinator m_coordinator;

    Context*    m_context;
    TimerNode   m_node;      // UserspaceQueue backing
    io::Handle  m_handle;    // KernelPerTimer backing
    Interval    m_interval;
    Interval    m_slack;
};

// Kill-aware sleep. Returns Ok if the sleep completed normally, Killed if the context was killed
// before the interval elapsed. A non-zero slack opts the deadline into quantization (see the
// covenant above); the default is exact.
//
SleepResult Sleep(Context* ctx, Interval interval, Interval slack = Interval::zero());

SleepResult Sleep(Interval interval, Interval slack = Interval::zero());

} // end namespace time
} // end namespace coop
