#pragma once

#include <csetjmp>
#include <cstdint>

#include "embedded_list.h"
#include "execution_handle.h"
#include "scheduler_state.h"
#include "spawn_configuration.h"

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
struct Manager;

// An ExecutionContext is what code runs "in."
//
struct ExecutionContext : EmbeddedListHookups<ExecutionContext, int, 0>
                        , EmbeddedListHookups<ExecutionContext, int, 1>
{
    // Embedded lists for tracking the set of lists that execution contexts can never be in more
    // than one of, e.g. there are multiple `ContextStateList`s in the manager, but contexts are
    // never in both active and yielded, etc. Or in two different manager's active list...
    //
    using AllContextsList = EmbeddedList<ExecutionContext, int, 0>;
    using ContextStateList = EmbeddedList<ExecutionContext, int, 1>;

	ExecutionContext(
        SpawnConfiguration const& config,
        ExecutionHandle* handle,
        Manager* manager)
    : m_blockingOn(nullptr)
    , m_blockingBehind(nullptr)
    , m_handle(handle)
	, m_state(SchedulerState::YIELDED)
	, m_priority(config.priority)
    , m_currentPriority(config.priority)
    , m_manager(manager)
    , m_killed(false)
	{
        if (m_handle)
        {
            m_handle->m_executionContext = this;
        }
		m_segment.m_size = config.stackSize;
	}

    ~ExecutionContext()
    {
        if (m_handle)
        {
            m_handle->m_executionContext = nullptr;
        }
    }

    // Return control to the manager so that it can schedule other contexts
    //
    bool Yield(bool force = false);

    Manager* GetManager()
    {
        return m_manager;
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
    ExecutionContext* m_blockingBehind;
    
    // Enter the block caused by the given coordinator
    //
    void Block(Coordinator* c);

    // Unblock the given context, switching to it if requested.
    //
    void Unblock(ExecutionContext* c, const bool schedule);

  private:
    friend struct ExecutionHandle;
    void Kill()
    {
        m_killed = true;
    }

  public:
    ExecutionHandle* m_handle;
	SchedulerState m_state;
	int m_priority;
	int m_currentPriority;
    Manager* m_manager;
    bool m_killed;

    // The jmp_buf operates as the 'bookmark' to jump back into when the context is active.
    //
	std::jmp_buf m_jmpBuf;

	// Must be last member
	//
	Segment m_segment;
};

