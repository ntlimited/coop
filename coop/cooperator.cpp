#include <mutex>
#include <new>
#include <thread>
#include <stdio.h>
#include <string.h>

#include "cooperator.h"
#include "detail/context_switch.h"
#include "launchable.h"

namespace coop
{

std::atomic<bool>        Cooperator::s_registryShutdown{false};
std::mutex               Cooperator::s_registryMutex;
Cooperator::RegistryList Cooperator::s_registry;

Cooperator::Cooperator(CooperatorConfiguration const& config)
: m_lastRdtsc(0)
, m_ticks(0)
, m_shutdown(false)
, m_uring(config.uring)
, m_scheduled(nullptr)
, m_submissionSemaphore(0)
, m_submissionAvailabilitySemaphore(8)
{
}

Cooperator::~Cooperator()
{
    std::lock_guard<std::mutex> lock(s_registryMutex);
    if (!Disconnected())
    {
        s_registry.Remove(this);
    }
}

void Cooperator::Shutdown()
{
    m_shutdown.store(true, std::memory_order_relaxed);

    // Wake the cooperator if it is blocked waiting for submissions
    //
    m_submissionSemaphore.release();

    // Wake anyone blocked in Submit. The released caller will see m_shutdown and chain-release
    // so that all blocked callers eventually drain out.
    //
    m_submissionAvailabilitySemaphore.release();
}

void Cooperator::ShutdownAll()
{
    // Close the gate so no new cooperators can register, then take the lock. Any Launch() that
    // beat us to the lock will have registered and be visible in the list; any Launch() that
    // follows will see the flag under the lock and shut itself down.
    //
    s_registryShutdown.store(true, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(s_registryMutex);
    s_registry.Visit([](Cooperator* co) -> bool
    {
        co->Shutdown();
        return true;
    });
}

void Cooperator::ResetGlobalShutdown()
{
    std::lock_guard<std::mutex> lock(s_registryMutex);
    assert(s_registry.IsEmpty());
    s_registryShutdown.store(false, std::memory_order_relaxed);
}

bool Cooperator::Submit(Submission func, void* arg, SpawnConfiguration const& config /* = s_defaultConfiguration */)
{
    m_submissionAvailabilitySemaphore.acquire();

    if (m_shutdown.load(std::memory_order_relaxed))
    {
        // Chain-release so the next blocked caller also wakes up and drains out
        //
        m_submissionAvailabilitySemaphore.release();
        return false;
    }

    m_submissionLock.lock();
    auto ret =  m_submissions.Push(ExecutionSubmission{func, arg, config});
    m_submissionLock.unlock();
    if (ret)
    {
        m_submissionSemaphore.release();
    }
    return ret;
}

void Cooperator::HandleCooperatorResumption(const SchedulerJumpResult res)
{
    // Charge elapsed time to the context that was running, then mark the cooperator's timestamp
    // for its own accounting. This single rdtsc serves both purposes.
    //
    auto now = rdtsc();
    m_scheduled->m_statistics.ticks += now - m_scheduled->m_lastRdtsc;
    m_lastRdtsc = now;

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
            m_stackPool.Free(m_scheduled, m_scheduled->m_segment.Size());
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
    {
        std::lock_guard<std::mutex> lock(s_registryMutex);
        if (s_registryShutdown.load(std::memory_order_relaxed))
        {
            Shutdown();
            return;
        }
        s_registry.Push(this);
    }

    Cooperator::thread_cooperator = this;
    m_lastRdtsc = rdtsc();

    m_uring.Init();

    bool shutdownKillDone = false;

    while (!m_yielded.IsEmpty() || !m_shutdown.load(std::memory_order_relaxed)
                                 || !shutdownKillDone
                                 || m_uring.PendingOps() > 0)
    {
        // When shutdown is requested, kill all live contexts from within the cooperator's
        // thread so they can exit naturally. This only needs to happen once; killed contexts
        // will check IsKilled() when resumed and return.
        //
        if (m_shutdown.load(std::memory_order_relaxed) && !shutdownKillDone)
        {
            shutdownKillDone = true;
            Spawn([this](Context* killCtx)
            {
                m_contexts.Visit([&](Context* c) -> bool
                {
                    if (c != killCtx && !c->IsKilled())
                    {
                        killCtx->Kill(c, false /* schedule */);
                    }
                    return true;
                });
            });
        }

        if (m_yielded.IsEmpty())
        {
            // Poll the native uring — contexts may be waiting on IO completions that would
            // move them from blocked to yielded.
            //
            m_uring.Poll();
            if (!m_yielded.IsEmpty())
            {
                continue;
            }

            // If there are blocked contexts, they may be waiting on IO that hasn't completed
            // yet. Keep spinning on uring completions (same behavior as the old dedicated uring
            // context). Only block on submissions when there's truly nothing in flight.
            //
            if (!m_blocked.IsEmpty())
            {
                SpawnSubmitted(false);
                continue;
            }
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
            auto* ctx = m_yielded.Pop();

            Resume(ctx);
            m_uring.Poll();

            if (!m_submissions.IsEmpty())
            {
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_registryMutex);
        s_registry.Remove(this);
    }
}

void Cooperator::YieldFrom(Context* ctx)
{
    auto ret = ContextSwitch(&ctx->m_sp, m_sp, static_cast<int>(SchedulerJumpResult::YIELDED));
    assert(static_cast<SchedulerJumpResult>(ret) == SchedulerJumpResult::RESUMED);
}

void Cooperator::Resume(Context* ctx)
{
    // We must be running this from "within" the cooperator vs with a running context
    //
    assert(!m_scheduled);
    assert(ctx->m_state == SchedulerState::YIELDED);

    ctx->m_state = SchedulerState::RUNNING;
    m_scheduled = ctx;

    auto ret = ContextSwitch(&m_sp, ctx->m_sp, static_cast<int>(SchedulerJumpResult::RESUMED));
    HandleCooperatorResumption(static_cast<SchedulerJumpResult>(ret));
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

    auto ret = ContextSwitch(&ctx->m_sp, m_sp, static_cast<int>(SchedulerJumpResult::BLOCKED));
    assert(static_cast<SchedulerJumpResult>(ret) == SchedulerJumpResult::RESUMED);
}

void Cooperator::Unblock(Context* ctx, const bool schedule)
{
    // When schedule is true, we need a currently running context to yield from. When false (e.g.
    // CQE processing from the cooperator loop), we just move the context to the yielded list.
    //
    assert(!schedule || m_scheduled);
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

    auto* prev = m_scheduled;

    auto now = rdtsc();
    prev->m_statistics.ticks += now - prev->m_lastRdtsc;

    prev->m_state = SchedulerState::YIELDED;
    m_yielded.Push(prev);

    ctx->m_state = SchedulerState::RUNNING;
    m_scheduled = ctx;
    ctx->m_lastRdtsc = now;

    auto ret = ContextSwitch(&prev->m_sp, ctx->m_sp, static_cast<int>(SchedulerJumpResult::RESUMED));
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
    if (!m_submissions.Pop(submitted))
    {
        // Woken by Shutdown() releasing the semaphore, not by an actual submission
        //
        m_submissionLock.unlock();
        return false;
    }

    m_submissionLock.unlock();
    m_submissionAvailabilitySemaphore.release();

    Spawn(submitted.config, [&](Context* ctx)
    {
        submitted.func(ctx, submitted.arg);
    });

    return true;
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

} // end namespace coop (resumed below after extern "C" trampoline)

extern "C" void CoopContextEntry(coop::Context* ctx)
{
    ctx->m_entry(ctx);

    // Run Launchable destructor (if any) while the context is still alive and schedulable,
    // since destruction may do cooperative IO (e.g. Descriptor Close).
    //
    if (ctx->m_cleanup)
    {
        ctx->m_cleanup(ctx);
    }

    // Context destructor must run while we're still on this context's stack so that kill signals
    // can context-switch to waiters safely.
    //
    ctx->~Context();

    void* dummy;
    ContextSwitch(
        &dummy,
        coop::Cooperator::thread_cooperator->m_sp,
        static_cast<int>(coop::SchedulerJumpResult::EXITED));
}

namespace coop
{

void Cooperator::EnterContext(Context* ctx)
{
    auto now = rdtsc();

    Context* lastCtx = m_scheduled;
    void** save_sp = lastCtx ? &lastCtx->m_sp : &m_sp;
    bool isSelf = !lastCtx;

    if (lastCtx)
    {
        lastCtx->m_statistics.ticks += now - lastCtx->m_lastRdtsc;
        lastCtx->m_state = SchedulerState::YIELDED;
        m_yielded.Push(lastCtx);
        m_scheduled = nullptr;
    }
    else
    {
        m_ticks += now - m_lastRdtsc;
    }

    ctx->m_state = SchedulerState::RUNNING;
    m_scheduled = ctx;
    ctx->m_lastRdtsc = now;

#ifndef NDEBUG
    SanityCheck();
#endif

    // Prepare the new context's stack for first entry via ContextSwitch
    //
    void* init_sp = ContextInit(ctx->m_segment.Top(), ctx);
    auto ret = ContextSwitch(save_sp, init_sp, 0);

    if (isSelf)
    {
        HandleCooperatorResumption(static_cast<SchedulerJumpResult>(ret));
    }
    else
    {
        assert(lastCtx == m_scheduled);
    }
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

        if (!Submit(&BoundaryCrossingKill, &args))
        {
            // Cooperator is shutting down; the kill will be handled by the shutdown sweep
            //
            return;
        }

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

thread_local Cooperator* Cooperator::thread_cooperator;

} // end namespace coop
