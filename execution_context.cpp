#include "execution_context.h"
#include "iomgr.h"

bool ExecutionContext::Yield(const bool force /* = false */)
{
    if (!force && !--m_currentPriority)
    {
        return false;
    }
    m_currentPriority = m_priority;

    m_manager->YieldFrom(this);
    return true;
}

void ExecutionContext::Block(Coordinator*)
{
    m_manager->Block(this);
}

void ExecutionContext::Unblock(ExecutionContext* other, const bool schedule)
{
    // `this` is the currently executing context (as always), `other` is the context to schedule
    // over this one
    //
    m_manager->Unblock(other, schedule);
}
