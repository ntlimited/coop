#include "sleep.h"

#include <time.h>

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
, m_handle(ctx, ctx->GetCooperator()->GetUring(), &m_coordinator)
, m_interval(interval)
, m_slack(slack)
{
}

Sleeper::~Sleeper()
{
    // io::Handle destructor cancels the pending timeout if it hasn't fired yet
    //
}

Coordinator* Sleeper::GetCoordinator()
{
    return &m_coordinator;
}

Interval Sleeper::Quantize(Interval interval) const
{
    if (m_slack <= Interval::zero())
    {
        return interval;
    }

    // Align the absolute deadline to a slack boundary so that concurrent sleeps sharing a slack grid
    // resolve to the same kernel expiry. io_uring's relative timeout counts from CLOCK_MONOTONIC at
    // submission, so the boundary is computed against that same clock; the extra wait handed to the
    // kernel is (rounded_deadline - now), the original interval plus at most one slack tick.
    //
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t nowUs      = static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
    const int64_t slackUs    = m_slack.count();
    const int64_t deadlineUs = nowUs + interval.count();
    const int64_t roundedUs  = ((deadlineUs + slackUs - 1) / slackUs) * slackUs;
    return Interval(roundedUs - nowUs);
}

bool Sleeper::Submit()
{
    return io::Timeout(m_handle, Quantize(m_interval));
}

bool Sleeper::Wait()
{
    auto result = CoordinateWithKill(m_context, &m_coordinator);
    return !result.Killed();
}

SleepResult Sleeper::Sleep()
{
    if (!Submit())
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
