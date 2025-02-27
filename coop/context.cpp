#include "context.h"
#include "cooperator.h"
#include "coordinator_extension.h"

namespace coop
{

Context::~Context()
{
    if (m_parent)
    {
        Detach();
    }
    // Properly kill this if we were not already. Note that this propagates down to child contexts
    //
    if (m_killedCoordinator.IsHeld())
    {
        Kill(this);
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

void Context::Kill(Context* other, const bool schedule /* = true */)
{
    CoordinatorExtension().Shutdown(&other->m_killedCoordinator, other);
    other->m_children.Visit([&](Context* child)
    {
        Kill(child, schedule);
        return true;
    });
}

} // end namespace coop
