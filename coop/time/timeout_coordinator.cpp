#include "driver.h"
#include "timeout_coordinator.h"

#include "coop/context.h"
#if 0
namespace coop
{

namespace time
{

TimeoutCoordinator::TimeoutCoordinator(Interval interval, Coordinator* wrap)
: m_interval(interval)
, m_wrapped(wrap)
{
}

bool TimeoutCoordinator::TryAcquire(Driver* driver, Context* ctx)
{
    if (m_wrapped->TryAcquire(ctx))
    {
        return true;
    }

    // Add ourselves to the blocklist for the wrapped coordinator, and set up our timeout to block us
    //
    AddAsBlocked(m_wrapped, ctx);
    m_timeout.Acquire(ctx);

    // We now will:
    // - Submit the timer task such that `m_timeout` will be released when the deadline passes
    // - Actually wait on m_timeout so that this will wake us up
    // 
    // Because we are in the blockedlist for the wrapped Coordinator, either coordinator will be
    // able to unblock us. We can then check which one did manage to, and cancel the other either
    // by directly removing ourselves from the wrpaped coordinator, or by disabling the timeout
    // wakeup task so we can safely terminate lifetimes.
    //
    
    Handle timeoutHandle(m_interval, &m_timeout);
    timeoutHandle.Submit(driver);
    m_timeout.Acquire(ctx);
    
    // We were either unblocked by the wrapped coordinator and still hold this, or we got unblocked
    // by the timer and then grabbed it again, so we can release it either way.
    //
    m_timeout.Release(ctx);
    
    if (HeldBy(m_wrapped, ctx))
    {
        // We are holding the lock we wanted to be holding and the timeout didn't trigger first
        //
        timeoutHandle.Cancel(ctx);
        return true;
    }

    return false;
}

} // end namespace coop::time
} // end namespace coop
#endif
