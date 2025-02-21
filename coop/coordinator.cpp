#include <cassert>

#include "coordinator.h"

#include "context.h"

namespace coop
{

Coordinator::Coordinator()
: m_heldBy(nullptr)
, m_head(nullptr)
, m_tail(nullptr)
{
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

    // Put on the tail of the linkedlist. We could do priority shenanigans here at some point
    //
    if (m_head == nullptr)
    {
        m_head = ctx;
        m_tail = ctx;
    }
    else
    {
        m_tail->m_blockingBehind = ctx;
        m_tail = ctx;
    }

    // Block the context on the this coordinator
    //
    ctx->Block(this);
}

void Coordinator::Release(Context* ctx, const bool schedule /* = true */)
{
    assert(m_heldBy);
    if (m_head)
    {
        // Pop the head off the list and pass ownership of the coordinator.
        //
        auto* unblock = m_head;
        m_heldBy = unblock;
        m_head = m_head->m_blockingBehind;
        ctx->Unblock(unblock, schedule);
    }
}

bool CoordinatedSemaphore::TryAcquire(Context* ctx)
{
    if (m_avail > 0)
    {
        // Transitioning from 1 to 0 means taking the coordinator
        //
        if (!--m_avail)
        {
            m_coordinator.Acquire(ctx);
        }
        return true;
    }
    
    return false;
}

void CoordinatedSemaphore::Acquire(Context* ctx)
{
    if (TryAcquire(ctx))
    {
        return;
    }

    // m_avail is 0 or lower and the coordinator is held
    //

    m_avail--;
    m_coordinator.Acquire(ctx);
}

void CoordinatedSemaphore::Release(Context* ctx)
{
    // If the avail count is 0 or higher, there are no waiters on the coordinator
    //
    if (m_avail++ >= 0)
    {
        // m_avail is now 1 or greater
        //
        return;
    }

    m_coordinator.Release(ctx);
}

} // end namespace coop
