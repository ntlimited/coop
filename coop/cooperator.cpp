#include <csetjmp>
#include <mutex>
#include <new>
#include <thread>
#include <stdio.h>
#include <string.h>

#include "cooperator.h"
#include "launchable.h"

#include "time/ticker.h"

namespace coop
{

bool Cooperator::Launch(
    Launchable& launch,
    Handle* handle /* = nullptr */)
{
    return Launch(s_defaultConfiguration, launch, handle);
} 

bool Cooperator::Launch(
        SpawnConfiguration const& config,
        Launchable& launch,
        Handle* handle /* = nullptr */)
{
    return Spawn(config, [&launch](Context* ctx)
    {
        launch.Launch(ctx);
    }, handle /* out */);
}

bool Cooperator::Submit(Submission func, void* arg, SpawnConfiguration const& config /* = s_defaultConfiguration */)
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

void Cooperator::HandleCooperatorResumption(const SchedulerJumpResult res)
{
    switch (res)
    {
        // The point is that this gets called in the other cases
        //
        case SchedulerJumpResult::DEFAULT:
        // This is never used to jump back into the cooperator
        //
        case SchedulerJumpResult::RESUMED:
        {
            assert(false);
        }
        case SchedulerJumpResult::EXITED:
        {
            free(m_scheduled);
            break;
        }
        case SchedulerJumpResult::YIELDED:
        {
            m_scheduled->m_state = SchedulerState::YIELDED;
			m_yielded.Push(m_scheduled);
            break;
        }
        case SchedulerJumpResult::BLOCKED:
        {
            m_scheduled->m_state = SchedulerState::BLOCKED;
            m_blocked.Push(m_scheduled);
            break;
        }
    }
    m_scheduled = nullptr;
    //SanityCheck();
}

void Cooperator::Launch()
{
    Cooperator::thread_cooperator = this;

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
            Context* ctx;
            m_yielded.Pop(ctx);

            Resume(ctx);

            if (!m_submissions.IsEmpty())
            {
                break;
            }
        }
	}
}

void Cooperator::YieldFrom(Context* ctx)
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

void Cooperator::Resume(Context* ctx)
{
    // We must be running this from "within" the cooperator vs with a running context
    //
    assert(!m_scheduled);
    assert(ctx->m_state == SchedulerState::YIELDED);
   
    ctx->m_state = SchedulerState::RUNNING;
    m_scheduled = ctx;

    //SanityCheck();
   
    auto ret = setjmp(m_jmpBuf);
    if (!ret)
    {
        LongJump(ctx->m_jmpBuf, SchedulerJumpResult::RESUMED);
    }
    else
    {
        HandleCooperatorResumption((static_cast<SchedulerJumpResult>(ret)));
    }
}

void Cooperator::Block(Context* ctx)
{
    // Someone else is registering a (yielded) context as being blocked
    //
    if (m_scheduled != ctx)
    {
        assert(ctx->m_state == SchedulerState::YIELDED);
        ctx->m_state = SchedulerState::BLOCKED;
        m_yielded.Remove(ctx);
        m_blocked.Push(ctx);

        //SanityCheck();
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

void Cooperator::Unblock(Context* ctx, const bool schedule)
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

bool Cooperator::SpawnSubmitted(bool wait /* = false */)
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

	Spawn(submitted.config, [&](Context* ctx)
    {
		submitted.func(ctx, submitted.arg);
    });

    //SanityCheck();

    return true;
}

bool Cooperator::SetTicker(time::Ticker* t)
{
    if (!Launch(*t, &m_tickerHandle))
    {
        return false;
    }
    m_ticker = t;
    return true;
}

time::Ticker* Cooperator::GetTicker()
{
    return m_ticker;
}

void Cooperator::SanityCheck()
{
    int yielded = 0;
    int blocked = 0;
    m_contexts.Visit([this, &yielded, &blocked](Context* ctx) -> bool
    {
        switch (ctx->m_state)
        {
            case SchedulerState::YIELDED:
                yielded++;
                break;
            case SchedulerState::BLOCKED:
                blocked++;
                break;
            case SchedulerState::RUNNING:
                assert(ctx == m_scheduled);
        }
        return true;
    });

    assert(yielded == YieldedCount());
    assert(blocked == BlockedCount());

    m_yielded.Visit([&yielded](Context* ctx) -> bool
    {
        yielded--;
        assert(ctx->m_state == SchedulerState::YIELDED);
        return true;
    });
    assert(yielded == 0);
    m_blocked.Visit([&blocked](Context* ctx) -> bool
    {
        blocked--;
        assert(ctx->m_state == SchedulerState::BLOCKED);
        return true;
    });
    assert(blocked == 0);
}

namespace
{

struct BoundaryCrossingKillArgs
{
    Handle* handle;
    std::mutex mutex;
};

void BoundaryCrossingKill(Context* taskCtx, void* args)
{
    auto* bc = reinterpret_cast<BoundaryCrossingKillArgs*>(args);

    taskCtx->GetCooperator()->BoundarySafeKill(bc->handle, true /* crossed */);

    bc->mutex.unlock();
}

} // end anonymous namespace

void Cooperator::BoundarySafeKill(Handle* handle, const bool crossed /* = false */)
{
    if (this == Cooperator::thread_cooperator)
    {
        // Context killing is something that Coordinators work for, and the Coordinator system
        // assumes we have a context at all times. This makes good semantic sense: the cooperator
        // 'native' context never coordinates with that mechanic.
        //
        // Eventually we will need (TODO) a shutdown/mass kill type operation where it will make
        // sense for the cooperator to be the one deciding to issue kill commands; this should be
        // refactored of course but in general I think it's actually a good pattern that the
        // cooperator can schedule work against itself; we should likely add a return system to
        // the Handle construct that allows for passing across the lives-in-a-context as well as
        // the lives-in-the-cooperator-thread boundaries.
        //
        assert(m_scheduled);
        if (!crossed)
        {
            // We were called originally from the cooperator's thread, so this is guaranteed safe
            //
            m_scheduled->Kill(handle->m_context);
        }
        else
        {
            // We transitioned into the cooperator's thread and can't guarantee that the context
            // was not destroyed during the jump. Instead, have to look at all the contexts to find
            // a match, and then (ABA) need to look at its handle, as that should not have been
            // reusable until this kill has completed and the owner that tried to kill with it
            // has the chance to consider reuse.
            //
            m_contexts.Visit([&](Context* check)
            {
                if (check != handle->m_context)
                {
                    return true;
                }
                    
                if (check->m_handle == handle)
                {
                    m_scheduled->Kill(handle->m_context);
                }

                // Either way, we're not going to find a match past this
                //
                return false;
            });
        }
        return;
    }

    BoundaryCrossingKillArgs args;
    args.mutex.lock();
    args.handle = handle;

    Submit(&BoundaryCrossingKill, &args);
    args.mutex.unlock();
}

void Cooperator::PrintContextTree(Context* ctx /* = nullptr */, int indent /* = 0 */)
{
    if (ctx)
    {
        for (int i = 0 ; i < indent ; i++)
        {
            printf("\t");
        }
        const char* status = ctx->m_state == coop::SchedulerState::RUNNING ? "Running" :
            ctx->m_state == SchedulerState::YIELDED ? "Yielded" : "Blocked";
        printf("%s (%p) [%s%s]\n", ctx->GetName(), ctx, status, ctx->IsKilled() ? "!" : "");
        ctx->m_children.Visit([&](coop::Context* child) -> bool
        {
            PrintContextTree(child, indent + 1);
            return true;
        });
        return;
    }

    size_t remaining = m_contexts.Size();
    printf("---- Context Tree (%lu) ----\n", remaining);
    m_contexts.Visit([&](coop::Context* child) -> bool
    {
        remaining--;
        if (!child->Parent())
        {
            PrintContextTree(child, 0);
        }
        return true;
    });
    assert(!remaining);
}

void DeleteContext(Context* ctx)
{
    memset(ctx, 0xD0, sizeof(Context));
    free(ctx);
}

void* AllocateContext(SpawnConfiguration const& config)
{
    // Enforce 128 byte alignment at both top and bottom
    //
    assert((config.stackSize & 127) == 0);
	return malloc(sizeof(Context) + config.stackSize);
}

void LongJump(std::jmp_buf& buf, SchedulerJumpResult result)
{
	longjmp(buf, static_cast<int>(result));
}

thread_local Cooperator* Cooperator::thread_cooperator;

} // end namespace coop
