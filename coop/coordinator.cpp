#include <cassert>

#include "coordinator.h"

#include "context.h"

namespace coop
{

Coordinator::Coordinator()
: m_heldBy(nullptr)
, m_shutdown(false)
{
}

bool Coordinator::IsHeld() const
{
    return !!m_heldBy;
}

bool Coordinator::TryAcquire(Context* ctx)
{
    if (m_shutdown)
    {
        return true;
    }

    if (m_heldBy == nullptr)
    {
        m_heldBy = ctx;
        return true;
    }

    return false;
}

void Coordinator::Acquire(Context* ctx)
{
    if (m_shutdown)
    {
        return;
    }

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
}

void Coordinator::Release(Context* ctx, const bool schedule /* = true */)
{
    if (m_shutdown)
    {
        return;
    }

    // We allow multiple release of shutdown coordinators as the acquire/release contract no
    // longer applies.
    //
    if (m_shutdown && !m_heldBy)
    {
        return;
    }

    m_heldBy = nullptr;

    // Pass control to the next in line blocked on the coordinator, if it exists.
    //
    Coordinated* next;
    if (!m_blocking.Pop(next))
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

// Shutting down a coordinator means that the instance will act as a 'signal': callers of Acquire
// APIs will be told they have taken the coordinator, and release calls will become no-ops.
//
// This is very useful for, e.g., kill conditions.
//
void Coordinator::Shutdown(Context* ctx)
{
    m_shutdown = true;
    m_heldBy = nullptr;

    // Unblock everyone coordinating on this instance
    //
    Coordinated* ord;
    while (m_blocking.Pop(ord))
    {
        ord->m_satisfied = true;
        ctx->Unblock(ord->GetContext());
    }
}

} // end namespace coop
