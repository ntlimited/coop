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
    m_lastRdtsc = rdtsc();

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
}

void Cooperator::Launch()
{
    Cooperator::thread_cooperator = this;
    m_lastRdtsc = rdtsc();

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
            m_lastRdtsc = rdtsc();

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

    auto ret = setjmp(m_jmpBuf);
    if (!ret)
    {
        m_ticks += rdtsc() - m_lastRdtsc;
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

    return true;
}

bool Cooperator::SetTicker(time::Ticker* t)
{
    m_ticker = t;
    return true;
}

time::Ticker* Cooperator::GetTicker()
{
    return m_ticker;
}

io::Uring* Cooperator::GetUring()
{
    return m_uring;
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
    Context::Handle* handle;
    std::mutex mutex;
};

void BoundaryCrossingKill(Context* taskCtx, void* args)
{
    // We cannot just do taskCtx->Kill(...) because we need to validate the handle/context. We could
    // refactor that logic here, technically, but it's got some nice clarity in its current form, imo.
    //
    auto* bc = reinterpret_cast<BoundaryCrossingKillArgs*>(args);
    taskCtx->GetCooperator()->BoundarySafeKill(bc->handle, true /* crossed */);
    bc->mutex.unlock();
}

} // end anonymous namespace

// BoundarySafeKill is invoked by Handles, which can be called within the cooperator's thread or
// from outside of it (boundary crossing).
//
void Cooperator::BoundarySafeKill(Context::Handle* handle, const bool crossed /* = false */)
{
    if (this != Cooperator::thread_cooperator)
    {
        assert(!crossed);

        // This is the "boundary crossing" part
        //
        BoundaryCrossingKillArgs args;
        args.mutex.lock();
        args.handle = handle;

        Submit(&BoundaryCrossingKill, &args);

        // This gets unlocked in the submitted func; technically we don't need to do this but
        // it preserves the same contract as when we kill from inside the boundary and don't
        // return until the acutal kill logic has gotten to run for the targetted context
        //
        args.mutex.lock();
        return;
    }

    if (crossed)
    {
        bool valid = false;

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
                valid = true;
            }

            // Either way, we're not going to find a match past this
            //
            return false;
        });
        if (!valid)
        {
            // Already gone
            //
            return;
        }
    }
    
    // We are now certain that (a) we're in the cooperator's thread and (b) we have a valid context
    // to kill
    //

    if (m_scheduled)
    {
        // We were called originally from the cooperator's thread, so this is guaranteed
        // safe
        //
        m_scheduled->Kill(handle->m_context);
    }
    else
    {
        // May need to plumb failure back through here; however this code path currently should
        // never happen because we don't kill handles from within the cooperator itself (today).
        //
        std::ignore = Spawn([&](Context* ctx)
        {
            ctx->Kill(handle->m_context);
        });
    }
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
        printf("%s (%p) [%s%s] {yields=%lu, blocks=%lu,ticks=%lu}\n",
            ctx->GetName(),
            ctx,
            status,
            ctx->IsKilled() ? "!" : "",
            ctx->m_statistics.yields,
            ctx->m_statistics.blocks,
            ctx->m_statistics.ticks);
        ctx->m_children.Visit([&](coop::Context* child) -> bool
        {
            PrintContextTree(child, indent + 1);
            return true;
        });
        return;
    }

    size_t remaining = m_contexts.Size();
    printf("---- Context Tree (%lu, ticks=%ld) ----\n", remaining, m_ticks);
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

int64_t Cooperator::rdtsc() const
{
    uint32_t hi, lo;
// cpuid forces a partial barrier preventing reordering, but we don't use rdtsc in a way that
// remotely cares about that.
//
#ifdef __NEVER__
    __asm__ __volatile__(
       "xorl %%eax,%%eax \n        cpuid" ::: "%rax", "%rcx", "%rdx");
#endif
    __asm__ __volatile__("rdtsc" : "=a"(lo),"=d"(hi));
    return static_cast<int64_t>((uint64_t)hi << 32 | lo);
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
