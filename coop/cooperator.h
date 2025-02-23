#pragma once

#include <cassert>
#include <csetjmp>
#include <mutex>
#include <semaphore>

#include "embedded_list.h"
#include "context.h"
#include "fixed_list.h"
#include "handle.h"
#include "spawn_configuration.h"

namespace coop
{

namespace time
{
    struct Ticker;
} // end namespace coop::time

enum class SchedulerJumpResult
{
	DEFAULT = 0,
	EXITED,
	YIELDED,
    BLOCKED,
    RESUMED,
};

struct Handle;
struct Launchable;

// A Cooperator manages multiple contexts
//
struct Cooperator
{
    thread_local static Cooperator* thread_cooperator;

	Cooperator()
    : m_shutdown(false)
    , m_ticker(nullptr)
	, m_scheduled(nullptr)
	, m_submissionSemaphore(0)
	, m_submissionAvailabilitySemaphore(8)
	{
	}

    Context* Scheduled()
    {
        return m_scheduled;
    }

    void Launch();

	// Spawn is an in-context API that code running under a cooperator is allowed to invoke
	// on its own cooperator, as that physical thread has uncontended access to the internals
    //
    // Under the hood 'Submit' is actually channeled into 'Spawn' by the cooperator, so this is
    // actually the entry point for all contexts.
	//
	template<typename Fn>
	bool Spawn(Fn const& fn, Handle* handle = nullptr);

	template<typename Fn>
	bool Spawn(
        SpawnConfiguration const& config,
        Fn const& fn,
        Handle* handle = nullptr);


    bool Launch(Launchable& launch, Handle* handle = nullptr);
    
    bool Launch(
        SpawnConfiguration const& config,
        Launchable& launch,
        Handle* handle = nullptr);

    using Submission = void(*)(Context*, void*);

	struct ExecutionSubmission
	{
		Submission func;
        void* arg;
		SpawnConfiguration config;
	};

	// Submit is the out-of-context version of Spawn that uses pthread_create style arguments
	// (doesn't get the sexy lambda syntax) because we need to pass control to the cooperator's
	// thread and can't guarantee lifetimes.
	//
	// This also (a) locks in a way that the cooperator will actually have to contend for in the
	// worst case and (b) has funky guarantees around scheduling; submitted executions are
	// periodically picked up by the cooperator while spawned executions have control immediately
	// passed to them.
	//
	// In general, this is expected to be used rarely.
	//
	// TODO some kind of more coherent guarantees (like that submissions are picked up on the
	// next context switch/yield)
	//
	bool Submit(Submission func, void* arg = nullptr, SpawnConfiguration const& config = s_defaultConfiguration);
   
    // Internal API for passing control to the given context
    //
    void Resume(Context* ctx);

    // The other half of `Yield()` which resumes into the cooperator's code
    //
    void YieldFrom(Context* ctx);

    void Block(Context* ctx);

    void Unblock(Context* ctx, const bool schedule);

    bool SpawnSubmitted(bool wait = false);

    // TODO do this properly and add more nuances around in vs out of cooperator-safe ooepratins.
    //
    void Shutdown()
    {
        m_shutdown = true;
    }

    bool SetTicker(time::Ticker*);

    time::Ticker* GetTicker();

    size_t ContextsCount() const
    {
        return m_contexts.Size();
    }

    size_t YieldedCount() const
    {
        return m_yielded.Size();
    }

    size_t BlockedCount() const
    {
        return m_blocked.Size();
    }

    void SanityCheck();

    // Ideally this would be better protected
    //
    void BoundarySafeKill(Handle*, const bool crossed = false);
   
    // Wow this is dirty, see the hpp
  private:

    void HandleCooperatorResumption(const SchedulerJumpResult res);

    bool m_shutdown;

	Context* m_scheduled;
    time::Ticker* m_ticker;
    Handle m_tickerHandle;

	std::jmp_buf m_jmpBuf;

	std::counting_semaphore<8> m_submissionSemaphore;
	std::counting_semaphore<8> m_submissionAvailabilitySemaphore;
	std::mutex m_submissionLock;

    FixedList<ExecutionSubmission, 8> m_submissions;

    // TODO this is a super dirty thing where the context removes itself from the
    // contexts list when it destructs.
    //
    friend struct Context;
    Context::AllContextsList m_contexts;
    Context::ContextStateList m_yielded;
    Context::ContextStateList m_blocked;
};

void* AllocateContext(SpawnConfiguration const& config);

void LongJump(std::jmp_buf& buf, SchedulerJumpResult result);

} // end namespace coop

#include "cooperator.hpp"
