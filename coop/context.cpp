#include "context.h"
#include "cooperator.h"

namespace coop
{

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
