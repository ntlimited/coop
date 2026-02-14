#include <cassert>

#include "coordinator.h"

#include "context.h"

namespace coop
{

Coordinator::Coordinator()
: m_heldBy(nullptr)
{
}

bool Coordinator::IsHeld() const
{
    return !!m_heldBy;
}

bool Coordinator::TryAcquire(Context* ctx)
{
    if (m_heldBy == nullptr)
    {
        m_heldBy = ctx;
        return true;
    }

    return false;
}

void Coordinator::Acquire(Context* ctx)
{
    if (m_heldBy == nullptr)
    {
        m_heldBy = ctx;
        return;
    }

    Coordinated coord(ctx);
    AddAsBlocked(&coord);

    // Block the context on the this coordinator
    //
    ctx->Block();

    return;
}

void Coordinator::Flash(Context* ctx)
{
    if (!m_heldBy)
    {
        return;
    }
    Acquire(ctx);
    Release(ctx);
}

void Coordinator::Release(Context* ctx, const bool schedule /* = true */)
{
    // Allow no-op release when the coordinator is not held. This supports Signal::Notify which
    // clears m_heldBy before MultiCoordinator cleanup can call Release.
    //
    if (!m_heldBy)
    {
        return;
    }
    m_heldBy = nullptr;

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
    next->m_satisfied = true;
    m_heldBy = next->GetContext();

    ctx->Unblock(m_heldBy, schedule);
}

void Coordinator::AddAsBlocked(Coordinated* c)
{
    c->m_satisfied = false;
    m_blocking.Push(c);
}

void Coordinator::RemoveAsBlocked(Coordinated* c)
{
    m_blocking.Remove(c);
}

bool Coordinator::HeldBy(Context* ctx)
{
    return m_heldBy == ctx;
}

} // end namespace coop
