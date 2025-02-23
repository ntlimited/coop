#include "context.h"
#include "cooperator.h"
#include "coordinator_extension.h"

namespace coop
{

Context::~Context()
{
    // Anyone waiting on this to die will get signaled before it's actually dead. Per the Spawn
    // docs, this must be decoupled from
    //
    if (m_killedCoordinator.IsHeld())
    {
        CoordinatorExtension().Shutdown(&m_killedCoordinator, this);
    }

    RemoveRef();
    m_zeroSignal.Acquire(this);

    if (m_handle)
    {
        m_handle->m_context = nullptr;
    }

    m_cooperator->m_contexts.Remove(this);
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

void Context::Block()
{
    m_cooperator->Block(this);
}

void Context::Unblock(Context* other, const bool schedule /* = true */)
{
    // `this` is the currently executing context (as always), `other` is the context to schedule
    // over this one
    //
    m_cooperator->Unblock(other, schedule);
}

} // end namespace coop
