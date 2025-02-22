#include "context.h"
#include "cooperator.h"

namespace coop
{

Context::~Context()
{
    // Anyone waiting on this to die will get signaled before it's actually dead. Per the Spawn
    // docs, this must be decoupled from
    //
    if (m_killedCoordinator.IsHeld())
    {
        // We were not 'killed' officially and must alert anyone who was waiting on that
        //
        m_killedCoordinator.Release(this);

        // Once anyone waiting on our kill coordinator got signaled by it, they know that it is
        // invalid
        //
        m_killedCoordinator.Acquire(this);
    }

    RemoveRef();
    m_zeroSignal.Acquire(this);

    if (m_handle)
    {
        m_handle->m_context = nullptr;
    }
}

bool Context::Yield(const bool force /* = false */)
{
    if (!force && !--m_currentPriority)
    {
        return false;
    }
    m_currentPriority = m_priority;

    m_cooperator->YieldFrom(this);
    return true;
}

void Context::Block(Coordinator* c)
{
    m_blockingOn = c;
    m_cooperator->Block(this);
}

void Context::Unblock(Context* other, const bool schedule)
{
    // `this` is the currently executing context (as always), `other` is the context to schedule
    // over this one
    //
    other->m_blockingOn = nullptr;
    m_cooperator->Unblock(other, schedule);
}

} // end namespace coop
