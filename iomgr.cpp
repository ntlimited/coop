#include <csetjmp>
#include <new>
#include <thread>
#include <stdio.h>

#include "iomgr.h"

bool Manager::Submit(Submission func, void* arg, SpawnConfiguration const& config /* = s_defaultConfiguration */)
{
    // Block on there being a slot available to submit to. This means the bool return is
	// currently irrelevant, but this should get extended to support a timeout at least.
    //
	m_submissionAvailabilitySemaphore.acquire();

    m_submissionLock.lock();
	auto ret =  m_submissions.Push(ExecutionSubmission{func, arg, config});
    m_submissionLock.unlock();
	if (ret)
    {
		// Signal that there is work to be picked up.
        //
		m_submissionSemaphore.release();
	}
    return ret;
}

void Manager::HandleManagerResumption(const SchedulerJumpResult res)
{
    switch (res)
    {
        // The point is that this gets called in the other cases
        //
        case SchedulerJumpResult::DEFAULT:
        // This is never used to jump back into the manager
        //
        case SchedulerJumpResult::RESUMED:
        {
            assert(false);
        }
        case SchedulerJumpResult::EXITED:
        {
            printf("Context %p has exicted\n", m_scheduled);
            delete m_scheduled;
            m_executionContexts.Remove(m_scheduled);
            break;
        }
        case SchedulerJumpResult::YIELDED:
        {
            printf("Context %p has yielded\n", m_scheduled);
            m_scheduled->m_state = SchedulerState::YIELDED;
			m_yielded.Push(m_scheduled);
            break;
        }
        case SchedulerJumpResult::BLOCKED:
        {
            printf("Context %p has been blocked\n", m_scheduled);
            m_scheduled->m_state = SchedulerState::BLOCKED;
            m_blocked.Push(m_scheduled);
            break;
        }
    }
    m_scheduled = nullptr;
}

void Manager::Launch()
{
    Manager::thread_manager = this;

    while (!m_yielded.IsEmpty() || !m_shutdown)
    {
		if (m_yielded.IsEmpty())
        {
            SpawnSubmitted(true /* wait */);
			continue;
        }
        else
        {
            // Spawn all submitted tasks
            //
            while (SpawnSubmitted(false));
        }

        int remainingIterations = 16;
        while (remainingIterations && !m_yielded.IsEmpty())
        {
            remainingIterations--;

            // Pop off a context and resume it
            //
            ExecutionContext* ctx;
            m_yielded.Pop(ctx);

            Resume(ctx);

            if (!m_submissions.IsEmpty())
            {
                break;
            }
        }
	}
}

void Manager::YieldFrom(ExecutionContext* ctx)
{
    // This is where all jmpBuf saving for contexts occurs presently
    //
    auto ret = setjmp(ctx->m_jmpBuf);
    if (!ret)
    {
        LongJump(m_jmpBuf, SchedulerJumpResult::YIELDED);
    }
    assert(static_cast<SchedulerJumpResult>(ret) == SchedulerJumpResult::RESUMED);
    return;
}

void Manager::Resume(ExecutionContext* ctx)
{
    // We must be running this from "within" the manager vs with a running context
    //
    assert(!m_scheduled);
    assert(ctx->m_state == SchedulerState::YIELDED);
   
    ctx->m_state = SchedulerState::RUNNING;
    m_scheduled = ctx;
   
    auto ret = setjmp(m_jmpBuf);
    if (!ret)
    {
        LongJump(ctx->m_jmpBuf, SchedulerJumpResult::RESUMED);
    }
    else
    {
        HandleManagerResumption((static_cast<SchedulerJumpResult>(ret)));
    }
}

void Manager::Block(ExecutionContext* ctx)
{
    // Someone else is registering a (yielded) context as being blocked
    //
    if (m_scheduled != ctx)
    {
        assert(ctx->m_state == SchedulerState::YIELDED);
        ctx->m_state = SchedulerState::BLOCKED;
        m_yielded.Remove(ctx);
        m_blocked.Push(ctx);
        return;
    }

    // The currently running context is placing itself into a blocked state
    //

    auto ret = setjmp(ctx->m_jmpBuf);
    if (!ret)
    {
        LongJump(m_jmpBuf, SchedulerJumpResult::BLOCKED);

        // Unreachable
        //
        assert(false);
        return;
    }

    assert(static_cast<SchedulerJumpResult>(ret) == SchedulerJumpResult::RESUMED);
    return;
}

void Manager::Unblock(ExecutionContext* ctx, const bool schedule)
{
    // Currently, never gets run from outside of an execution context
    //
    assert(m_scheduled);
    assert(ctx->m_state == SchedulerState::BLOCKED);

    m_blocked.Remove(ctx);

    // If we are not scheduling it immediately, place it into the yielded state to be
    // scheduled organically later
    //
    if (!schedule)
    {
        ctx->m_state = SchedulerState::YIELDED;
        m_yielded.Push(ctx);
        return;
    }

    // Yield from the current context and jump into the unblocked one immediately if we were told
    // to schedule it.
    //

    printf("Unblocking %p from %p, yielding and resuming\n", ctx, m_scheduled);

    auto ret = setjmp(m_scheduled->m_jmpBuf);
    if (!ret)
    {
        m_scheduled->m_state = SchedulerState::YIELDED;
        m_yielded.Push(m_scheduled);

        ctx->m_state = SchedulerState::RUNNING;
        m_scheduled = ctx;
        
        LongJump(ctx->m_jmpBuf, SchedulerJumpResult::RESUMED);
        // unreachable
        //
        assert(false);
        return;
    }

    assert(static_cast<SchedulerJumpResult>(ret) == SchedulerJumpResult::RESUMED);
}

bool Manager::SpawnSubmitted(bool wait /* = false */)
{
    if (wait)
    {
        m_submissionSemaphore.acquire();
    }
    else
    {
        if (!m_submissionSemaphore.try_acquire())
        {
            return false;
        }
    }
	m_submissionLock.lock();

	ExecutionSubmission submitted;
    m_submissions.Pop(submitted);

	m_submissionLock.unlock();
    m_submissionAvailabilitySemaphore.release();

	Spawn(submitted.config, [&](ExecutionContext* ctx)
    {
		submitted.func(ctx, submitted.arg);
    });
    return true;
}

void* AllocateExecutionContext(SpawnConfiguration const& config)
{
    // Enforce 128 byte alignment at both top and bottom
    //
    assert((config.stackSize & 127) == 0);
	return malloc(sizeof(ExecutionContext) + config.stackSize);
}

void LongJump(std::jmp_buf& buf, SchedulerJumpResult result)
{
	longjmp(buf, static_cast<int>(result));
}

thread_local Manager* Manager::thread_manager;
