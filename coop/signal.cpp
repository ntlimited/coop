#include "signal.h"

#include "context.h"
#include "cooperator.h"

namespace coop
{

Signal::Signal(Context* owner)
: m_signaled(false)
, m_coord(owner)
{
}

bool Signal::IsSignaled() const
{
    return m_signaled;
}

void Signal::Wait(Context* ctx)
{
    if (m_signaled)
    {
        return;
    }
    m_coord.Acquire(ctx);
}

void Signal::Notify(Context* ctx, bool schedule /* = true */)
{
    m_signaled = true;
    m_coord.m_held = false;

    // Steal the blocking list into a local before unblocking anyone. When schedule=true,
    // Unblock context-switches to the waiter immediately; if that waiter exits, the context
    // owning this signal is freed and the coordinator's sentinel becomes inaccessible.
    //
    Coordinated::List local;
    local.Steal(m_coord.m_blocking);

    // Route every waiter through the cooperator's shared dispatch so continuation waiters are
    // honored here too (Notify previously assumed every waiter was a context).
    //
    while (auto* ord = local.Pop())
    {
        Cooperator::thread_cooperator->WakeWaiter(ord, schedule);
    }
}

} // end namespace coop
