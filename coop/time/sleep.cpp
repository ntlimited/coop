#include "sleep.h"

#include <time.h>

#include "now.h"

#include "coop/cooperator.h"
#include "coop/coordinate_with.h"
#include "coop/io/timeout.h"
#include "coop/self.h"

namespace coop
{

namespace time
{

Sleeper::Sleeper(Context* ctx, Interval interval, Interval slack)
: m_coordinator()
, m_context(ctx)
, m_node()
, m_handle(ctx, ctx->GetCooperator()->GetUring(), &m_coordinator)
, m_interval(interval)
, m_slack(slack)
{
}

Sleeper::~Sleeper()
{
    // Clean up whichever backing was armed. In the queue path the node is removed if still linked
    // (the kill path leaves it linked; a serviced sleep is already unlinked). In the kernel path the
    // io::Handle destructor cancels and drains a pending timeout. Both are no-ops when the other
    // backing was used, so the teardown is unconditional and allocation-free -- including on a
    // killed context.
    //
    m_context->GetCooperator()->CancelTimer(&m_node);
}

Coordinator* Sleeper::GetCoordinator()
{
    return &m_coordinator;
}

int64_t Sleeper::DeadlineUs() const
{
    // Ceil the start so the absolute deadline is never earlier than now+interval in real terms (see
    // MonotonicMicrosCeil) -- the queue path's guard against returning a sub-microsecond early.
    //
    const int64_t nowUs    = MonotonicMicrosCeil();
    const int64_t deadline = nowUs + m_interval.count();
    if (m_slack <= Interval::zero())
    {
        return deadline;
    }

    // Align the absolute deadline up to a slack boundary so concurrent sleeps sharing a slack grid
    // collapse onto the same queue deadline (and therefore the same kernel expiry).
    //
    const int64_t slackUs = m_slack.count();
    return ((deadline + slackUs - 1) / slackUs) * slackUs;
}

Interval Sleeper::QuantizedInterval() const
{
    if (m_slack <= Interval::zero())
    {
        return m_interval;
    }

    // io_uring's relative timeout counts from CLOCK_MONOTONIC at submission; round the absolute
    // deadline up to a slack boundary against that same clock and hand back the resulting interval.
    //
    const int64_t nowUs      = MonotonicMicros();
    const int64_t slackUs    = m_slack.count();
    const int64_t deadlineUs = nowUs + m_interval.count();
    const int64_t roundedUs  = ((deadlineUs + slackUs - 1) / slackUs) * slackUs;
    return Interval(roundedUs - nowUs);
}

bool Sleeper::Arm()
{
    Cooperator* co = m_context->GetCooperator();
    if (co->UsesTimerQueue())
    {
        // Hold the coordinator so the subsequent CoordinateWithKill blocks; the timer service
        // Releases it when the deadline expires. Mirrors how io::Handle acquires its coordinator at
        // submit. No SQE, so this cannot fail.
        //
        m_coordinator.TryAcquire(m_context);
        co->RegisterTimer(&m_node, DeadlineUs(), &m_coordinator);
        return true;
    }

    // Kernel-per-timer: arm a standalone IORING_OP_TIMEOUT whose CQE Releases the coordinator.
    //
    return io::Timeout(m_handle, QuantizedInterval());
}

bool Sleeper::Wait()
{
    auto result = CoordinateWithKill(m_context, &m_coordinator);
    return !result.Killed();
}

SleepResult Sleeper::Sleep()
{
    if (!Arm())
    {
        return SleepResult::Error;
    }
    return Wait() ? SleepResult::Ok : SleepResult::Killed;
}

SleepResult Sleep(Context* ctx, Interval interval, Interval slack)
{
    return Sleeper(ctx, interval, slack).Sleep();
}

SleepResult Sleep(Interval interval, Interval slack)
{
    return Sleep(Self(), interval, slack);
}

} // end namespace coop::time
} // end namespace coop
