#include "context.h"
#include "cooperator.h"
#include "coordinator_extension.h"

namespace coop
{

Context::Context(
        Context* parent,
        SpawnConfiguration const& config,
        Handle* handle,
        Cooperator* cooperator)
: m_parent(parent)
, m_handle(handle)
, m_state(SchedulerState::YIELDED)
, m_priority(config.priority)
, m_currentPriority(config.priority)
, m_cooperator(cooperator)
{
    m_killedCoordinator.Acquire(this);
    if (m_handle)
    {
        m_handle->m_context = this;
    }

    if (m_parent)
    {
        assert(!m_parent->IsKilled());
        m_parent->m_children.Push(this);
        m_parent->m_lastChild.TryAcquire(this);
    }
    m_segment.m_size = config.stackSize;

    m_statistics.ticks = 0;
    m_statistics.yields = 0;
    m_statistics.blocks = 0;
    m_lastRdtsc = m_cooperator->rdtsc();
}

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

    if (m_handle)
    {
        m_handle->m_context = nullptr;
    }

    // Wait on the last child before we allow the context to be cleaned up
    //
    m_lastChild.Acquire(this);
    m_cooperator->m_contexts.Remove(this);
}

bool Context::Yield(const bool force /* = false */)
{
    if (!force && !--m_currentPriority)
    {
        return false;
    }
    ++m_statistics.yields;
    m_currentPriority = m_priority;

    m_statistics.ticks += m_cooperator->rdtsc() - m_lastRdtsc;
    m_cooperator->YieldFrom(this);
    m_lastRdtsc = m_cooperator->rdtsc();
    return true;
}

void Context::Detach()
{
    assert(m_parent);
    m_parent->m_children.Remove(this);
    if (m_parent->m_children.IsEmpty())
    {
        m_parent->m_lastChild.Release(this, false /* schedule */);
    }
    m_parent = nullptr;
}

void Context::Block()
{
    ++m_statistics.blocks;
    m_statistics.ticks += m_cooperator->rdtsc() - m_lastRdtsc;
    m_cooperator->Block(this);
    m_lastRdtsc = m_cooperator->rdtsc();
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

    // TODO this is a pretty unsafe pattern and probably needs to not have the "schedule" mechanism.
    // Maybe it would be possible to split up the way it does this to get some fascimile of the
    // intended "actually let the thing fully die" way it was conceptually intended to.
    //
    other->m_children.Visit([&](Context* child)
    {
        Kill(child, schedule);
        return true;
    });
}

} // end namespace coop
