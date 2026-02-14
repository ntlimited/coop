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

    // Unblock everyone waiting on the signal
    //
    Coordinated* ord;
    while (m_coord.m_blocking.Pop(ord))
    {
        ord->m_satisfied = true;
        ctx->Unblock(ord->GetContext(), schedule);
    }
}

} // end namespace coop
