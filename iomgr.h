#pragma once

#include <cassert>
#include <csetjmp>
#include <mutex>
#include <semaphore>

#include "fixed_list.h"
#include "spawn_configuration.h"

enum class SchedulerJumpResult
{
	DEFAULT = 0,
	EXITED,
	YIELDED,
    BLOCKED,
    RESUMED,
};

struct ExecutionContext;
struct ExecutionHandle;

struct Manager
{
    thread_local static Manager* thread_manager;

	Manager()
	: m_scheduled(nullptr)
	, m_submissionSemaphore(0)
	, m_submissionAvailabilitySemaphore(8)
    , m_shutdown(false)
	{
	}

	void Launch();

	// Spawn is an in-context API that code running under a manager is allowed to invoke
	// on its own manager, as that physical thread has uncontended access to manager internals
    //
    // Under the hood 'Submit' is actually channeled into 'Spawn' by the manager, so this is
    // actually the entry point for all contexts.
	//
	template<typename Fn>
	bool Spawn(Fn const& fn, ExecutionHandle* handle = nullptr);

	template<typename Fn>
	bool Spawn(SpawnConfiguration const& config, Fn const& fn, ExecutionHandle* handle = nullptr);

    using Submission = void(*)(ExecutionContext*, void*);

	struct ExecutionSubmission
	{
		Submission func;
        void* arg;
		SpawnConfiguration config;
	};

	// Submit is the out-of-context version of Spawn that uses pthread_create style arguments
	// (doesn't get the sexy lambda syntax) because we need to pass control to the manager's
	// thread and can't guarantee lifetimes.
	//
	// This also (a) locks in a way that the manager will actually have to contend for in the
	// worst case and (b) has funky guarantees around scheduling; submitted executions are
	// periodically picked up by the manager while spawned executions have control immediately
	// passed to them.
	//
	// In general, this is expected to be used rarely.
	//
	// TODO some kind of more coherent guarantees (like that submissions are picked up on the
	// next context switch/yield)
	//
	bool Submit(Submission func, void* arg, SpawnConfiguration const& config = s_defaultConfiguration);
   
    // Internal API for passing control to the given context
    //
    void Resume(ExecutionContext* ctx);

    // The other half of `Yield()` which resumes into the manager's code
    //
    void YieldFrom(ExecutionContext* ctx);

    void Block(ExecutionContext* ctx);

    void Unblock(ExecutionContext* ctx, const bool schedule);

    bool SpawnSubmitted(bool wait = false);

    void Shutdown()
    {
        m_shutdown = true;
    }

  private:
    void HandleManagerResumption(const SchedulerJumpResult res);

    bool m_shutdown;

	ExecutionContext* m_scheduled;
	std::jmp_buf m_jmpBuf;

	std::counting_semaphore<8> m_submissionSemaphore;
	std::counting_semaphore<8> m_submissionAvailabilitySemaphore;
	std::mutex m_submissionLock;

	FixedList<ExecutionSubmission, 8> m_submissions;
	FixedList<ExecutionContext*, 128> m_executionContexts;
	FixedList<ExecutionContext*, 128> m_active;
	FixedList<ExecutionContext*, 128> m_yielded;
	FixedList<ExecutionContext*, 128> m_blocked;
};

void* AllocateExecutionContext(SpawnConfiguration const& config);

void LongJump(std::jmp_buf& buf, SchedulerJumpResult result);

#include "iomgr.hpp"
