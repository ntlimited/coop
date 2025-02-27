#pragma once

#include <csetjmp>
#include <cstdint>

#include "coordinator.h"
#include "channel.h"
#include "embedded_list.h"
#include "handle.h"
#include "ref.h"
#include "scheduler_state.h"
#include "spawn_configuration.h"

namespace coop
{

struct Segment
{
	size_t m_size;
	uint8_t	m_bottom[0] __attribute__((aligned(128)));
	
	size_t Size() const
	{
		return m_size;
	}

	void* Bottom()
	{
		return reinterpret_cast<void*>(&m_bottom[0]);
	}

	void* Top()
	{
		return reinterpret_cast<void*>(&m_bottom[m_size]);
	}
};

struct Coordinator;
struct CoordinatorExtension;
struct Cooperator;

// Three different groups of mutually exclusive lists are kept for contexts:
// - the list of all contexts for a given cooperator
// - the list of all contexts in a given state for a given cooperator
// - the list of all child contexts for a given cooperator
//
static constexpr int CONTEXT_LIST_ALL = 0;
static constexpr int CONTEXT_LIST_STATE = 1;
static constexpr int CONTEXT_LIST_CHILDREN = 2;

// An Context is what code runs "in," in cooperation with a Cooperator. Each
//
struct Context : EmbeddedListHookups<Context, int, CONTEXT_LIST_ALL>
               , EmbeddedListHookups<Context, int, CONTEXT_LIST_STATE>
               , EmbeddedListHookups<Context, int, CONTEXT_LIST_CHILDREN>
               , Reffed<Context>
{
    // Embedded lists for tracking the set of lists that contexts can never be in more
    // than one of, e.g. there are multiple `ContextStateList`s in the cooperator, but contexts are
    // never in both active and yielded, etc. Or in two different cooperator's active list...
    //
    using AllContextsList = EmbeddedList<Context, int, CONTEXT_LIST_ALL>;
    using ContextStateList = EmbeddedList<Context, int, CONTEXT_LIST_STATE>;
    using ContextChildrenList = EmbeddedList<Context, int, CONTEXT_LIST_CHILDREN>;

  protected:
    friend struct Cooperator;

	Context(
        Context* parent,
        SpawnConfiguration const& config,
        Handle* handle,
        Cooperator* cooperator)
    : Reffed<Context>(this)
    , m_parent(parent)
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
        }
		m_segment.m_size = config.stackSize;
	}
  public:

    ~Context();

    // Return control to the cooperator so that it can schedule other contexts
    //
    bool Yield(bool force = false);

    Cooperator* GetCooperator()
    {
        return m_cooperator;
    }

    // The Killed system for contexts (and presumably most things) functions using a coordinator
    // that initially is held by the context itself, and then shutdown as a kill signal.
    //
    bool IsKilled() const
    {
        
        return !m_killedCoordinator.IsHeld();
    }

    Coordinator* GetKilledSignal()
    {
        return &m_killedCoordinator;
    }

    Context* Parent()
    {
        return m_parent;
    }

    const char* GetName() const
    {
        return m_name ? m_name : "[anonymous]";
    }

    void SetName(const char* name)
    {
        m_name = name;
    }

    // Detach disassociates the context from its parent so that it will not be killed when the
    // parent is. If you have a reason to use this, it is assumed you're able to make sure no
    // there's a coherent contract where no one else is going to or already detached it.
    //
    void Detach()
    {
        assert(m_parent);
        m_parent->m_children.Remove(this);
        m_parent = nullptr;
    }

  private:
    friend struct Cooperator;
    friend struct Coordinator;
    friend struct CoordinatorExtension;

    // Enter the block caused by the given coordinator
    //
    void Block();

    // Unblock the given context, switching to it if requested.
    //
    void Unblock(Context* c, const bool schedule = true);

  private:
    friend struct Handle;
    
    // Keeping with the concept that all Context APIs should only be called when it is actively
    // scheduled in the cooperator, this kills the other given context. In practice, it's also
    // simply necessary that we leverage the current context to coordinate downstream events.
    //
    void Kill(Context* other, const bool schedule = true);

  public:
    Context* m_parent;
    Handle* m_handle;
	SchedulerState m_state;
	int m_priority;
	int m_currentPriority;
    Cooperator* m_cooperator;
    Coordinator m_killedCoordinator;
    ContextChildrenList m_children;
    const char* m_name;

    // The jmp_buf operates as the 'bookmark' to jump back into when the context is active.
    //
	std::jmp_buf m_jmpBuf;

	// Must be last member
	//
	Segment m_segment;
};

} // end namespace coop
