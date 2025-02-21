#pragma once

#include <csetjmp>
#include <cstdint>

#include "embedded_list.h"
#include "handle.h"
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

// An Context is what code runs "in."
//
struct Context : EmbeddedListHookups<Context, int, 0>
                        , EmbeddedListHookups<Context, int, 1>
{
    // Embedded lists for tracking the set of lists that contexts can never be in more
    // than one of, e.g. there are multiple `ContextStateList`s in the cooperator, but contexts are
    // never in both active and yielded, etc. Or in two different cooperator's active list...
    //
    using AllContextsList = EmbeddedList<Context, int, 0>;
    using ContextStateList = EmbeddedList<Context, int, 1>;

	Context(
        SpawnConfiguration const& config,
        Handle* handle,
        Cooperator* cooperator)
    : m_blockingOn(nullptr)
    , m_blockingBehind(nullptr)
    , m_handle(handle)
	, m_state(SchedulerState::YIELDED)
	, m_priority(config.priority)
    , m_currentPriority(config.priority)
    , m_cooperator(cooperator)
    , m_killed(false)
	{
        if (m_handle)
        {
            m_handle->m_context = this;
        }
		m_segment.m_size = config.stackSize;
	}

    ~Context()
    {
        if (m_handle)
        {
            m_handle->m_context = nullptr;
        }
    }

    // Return control to the cooperator so that it can schedule other contexts
    //
    bool Yield(bool force = false);

    Cooperator* GetCooperator()
    {
        return m_cooperator;
    }

    bool IsKilled() const
    {
        return m_killed;
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
        m_killed = true;
    }

  public:
    Handle* m_handle;
	SchedulerState m_state;
	int m_priority;
	int m_currentPriority;
    Cooperator* m_cooperator;
    bool m_killed;

    // The jmp_buf operates as the 'bookmark' to jump back into when the context is active.
    //
	std::jmp_buf m_jmpBuf;

	// Must be last member
	//
	Segment m_segment;
};

} // end namespace coop
