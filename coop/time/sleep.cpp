#include "sleep.h"

#include "coop/cooperator.h"
#include "coop/coordinate_with.h"
#include "coop/io/timeout.h"
#include "coop/self.h"

namespace coop
{

namespace time
{

Sleeper::Sleeper(Context* ctx, Interval interval)
: m_coordinator()
, m_context(ctx)
, m_handle(ctx, ctx->GetCooperator()->GetUring(), &m_coordinator)
, m_interval(interval)
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

bool Sleeper::Submit()
{
    return io::Timeout(m_handle, m_interval);
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

SleepResult Sleep(Context* ctx, Interval interval)
{
    return Sleeper(ctx, interval).Sleep();
}

SleepResult Sleep(Interval interval)
{
    return Sleep(Self(), interval);
}

} // end namespace coop::time
} // end namespace coop
