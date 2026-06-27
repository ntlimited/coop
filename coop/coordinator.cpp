#include <cassert>

#include "coordinator.h"

#include "context.h"

namespace coop
{

Coordinator::Coordinator()
: m_held(false)
{
}

bool Coordinator::IsHeld() const
{
    return m_held;
}

bool Coordinator::TryAcquire(Context*)
{
    if (!m_held)
    {
        m_held = true;
        return true;
    }

    return false;
}

void Coordinator::Acquire(Context* ctx)
{
    if (!m_held)
    {
        m_held = true;
        return;
    }

    Coordinated coord(ctx);
    AddAsBlocked(&coord);

    // Block the context on this coordinator
    //
    ctx->Block();
}

void Coordinator::Flash(Context* ctx)
{
    if (!m_held)
    {
        return;
    }
    Acquire(ctx);
    Release(ctx);
}

void Coordinator::Release(Context* ctx, const bool schedule /* = true */)
{
    // Allow no-op release when the coordinator is not held. This supports Signal::Notify which
    // clears m_held before MultiCoordinator cleanup can call Release.
    //
    if (!m_held)
    {
        return;
    }
    m_held = false;

    // Pass control to the next in line blocked on the coordinator, if it exists.
    //
    auto* next = m_blocking.Pop();
    if (!next)
    {
        return;
    }

    // Mark the coordinated instance as having been satisfied by gaining the lock now
    // that we've popped it from the list
    //
    next->Satisfy();

    // A continuation waiter runs to completion here as a function call on the releasing
    // cooperator — no context switch, no ownership transfer. The coordinator stays released.
    //
    if (next->IsContinuation())
    {
        next->GetContinuation()->Resume(this);
        return;
    }

    // Hand the coordinator to the next waiter (it stays held) and make that context runnable.
    //
    m_held = true;
    ctx->Unblock(next->GetContext(), schedule);
}

void Coordinator::AddAsBlocked(Coordinated* c)
{
    c->Reset();
    m_blocking.Push(c);
}

void Coordinator::RemoveAsBlocked(Coordinated* c)
{
    m_blocking.Remove(c);
}

} // end namespace coop
