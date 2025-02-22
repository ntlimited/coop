#pragma once

#include <csetjmp>
#include <cstdint>

#include "coordinator.h"
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
struct Cooperator;

static constexpr int CONTEXT_LIST_ALL = 0;
static constexpr int CONTEXT_LIST_STATE = 1;

// An Context is what code runs "in."
//
struct Context : EmbeddedListHookups<Context, int, CONTEXT_LIST_ALL>
               , EmbeddedListHookups<Context, int, CONTEXT_LIST_STATE>
               , Reffed<Context>
{
    // Embedded lists for tracking the set of lists that contexts can never be in more
    // than one of, e.g. there are multiple `ContextStateList`s in the cooperator, but contexts are
    // never in both active and yielded, etc. Or in two different cooperator's active list...
    //
    using AllContextsList = EmbeddedList<Context, int, CONTEXT_LIST_ALL>;
    using ContextStateList = EmbeddedList<Context, int, CONTEXT_LIST_STATE>;

	Context(
        SpawnConfiguration const& config,
        Handle* handle,
        Cooperator* cooperator)
    : Reffed<Context>(this)
    , m_blockingOn(nullptr)
    , m_blockingBehind(nullptr)
    , m_handle(handle)
	, m_state(SchedulerState::YIELDED)
	, m_priority(config.priority)
    , m_currentPriority(config.priority)
    , m_cooperator(cooperator)
	{
        TakeRef();
        m_killedCoordinator.Acquire(this);
        if (m_handle)
        {
            m_handle->m_context = this;
        }
		m_segment.m_size = config.stackSize;
	}

    ~Context();

    // Return control to the cooperator so that it can schedule other contexts
    //
    bool Yield(bool force = false);

    Cooperator* GetCooperator()
    {
        return m_cooperator;
    }

    bool IsKilled() const
    {
        return !m_killedCoordinator.IsHeld();
    }

  private:
    friend struct Coordinator;

    // Blocking state: what resources that the 
    //
    Coordinator* m_blockingOn;
    Context* m_blockingBehind;
    
    // Enter the block caused by the given coordinator
    //
    void Block(Coordinator* c);

    // Unblock the given context, switching to it if requested.
    //
    void Unblock(Context* c, const bool schedule);

  private:
    friend struct Handle;
    void Kill()
    {
        m_killedCoordinator.Release(this);
    }

  public:
    Handle* m_handle;
	SchedulerState m_state;
	int m_priority;
	int m_currentPriority;
    Cooperator* m_cooperator;
    Coordinator m_killedCoordinator;

    // The jmp_buf operates as the 'bookmark' to jump back into when the context is active.
    //
	std::jmp_buf m_jmpBuf;

	// Must be last member
	//
	Segment m_segment;
};

} // end namespace coop
