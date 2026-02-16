#include "signal.h"

#include "context.h"

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
    m_coord.m_heldBy = nullptr;

    // Steal the blocking list into a local before unblocking anyone. When schedule=true,
    // Unblock context-switches to the waiter immediately; if that waiter exits, the context
    // owning this signal is freed and the coordinator's sentinel becomes inaccessible.
    //
    Coordinated::List local;
    local.Steal(m_coord.m_blocking);

    while (auto* ord = local.Pop())
    {
        ord->Satisfy();
        ctx->Unblock(ord->GetContext(), schedule);
    }
}

} // end namespace coop
