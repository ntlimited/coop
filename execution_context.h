#pragma once

#include <csetjmp>
#include <cstdint>

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
struct ExecutionContext
{
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

public:
    ExecutionHandle* m_handle;
	SchedulerState m_state;
	int m_priority;
	int m_currentPriority;
    Manager* m_manager;

    // The jmp_buf operates as the 'bookmark' to jump back into when the context is active.
    //
	std::jmp_buf m_jmpBuf;

	// Must be last member
	//
	Segment m_segment;
};

