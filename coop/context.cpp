#include "context.h"
#include "cooperator.h"

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
, m_killedSignal(this)
{
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
    m_entry = nullptr;

    m_statistics.ticks = 0;
    m_statistics.yields = 0;
    m_statistics.blocks = 0;
    m_lastRdtsc = 0;
}

Context::~Context()
{
    if (m_parent)
    {
        Detach();
    }

    // Properly kill this if we were not already. Note that this propagates down to child contexts
    //
    if (!m_killedSignal.IsSignaled())
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

    m_cooperator->YieldFrom(this);
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
    using ChildHookups = EmbeddedListHookups<Context, int, CONTEXT_LIST_CHILDREN>;

    // Iterative post-order traversal: fire signals bottom-up so children are killed before
    // parents. This avoids recursion, which would overflow the 16KB context stacks on deep
    // trees. All descendants use schedule=false (no context switches during traversal);
    // only the target itself uses the caller's schedule flag.
    //

    // Descend to the leftmost leaf
    //
    auto* ctx = other;
    while (!ctx->m_children.IsEmpty())
    {
        ctx = ctx->m_children.Peek();
    }

    // Process all descendants bottom-up
    //
    while (ctx != other)
    {
        ctx->m_killedSignal.Notify(ctx, false);

        // Move to next sibling, or ascend if this was the last child
        //
        auto* next = ctx->m_parent->m_children.Next(static_cast<ChildHookups*>(ctx));
        if (next)
        {
            ctx = next;
            while (!ctx->m_children.IsEmpty())
            {
                ctx = ctx->m_children.Peek();
            }
        }
        else
        {
            ctx = ctx->m_parent;
        }
    }

    other->m_killedSignal.Notify(other, schedule);
}

} // end namespace coop
