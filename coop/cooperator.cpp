#include <mutex>
#include <new>
#include <pthread.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cooperator.h"
#include "context_var.h"
#include "detail/context_switch.h"
#include "detail/memory_order.h"
#include "io/descriptor.h"
#include "io/read.h"
#include "launchable.h"
#include "perf/patch.h"
#include "perf/probe.h"
#include "perf/sampler.h"

namespace coop
{

std::atomic<bool>        Cooperator::s_registryShutdown{false};
std::mutex               Cooperator::s_registryMutex;
Cooperator::RegistryList Cooperator::s_registry;

Cooperator::Cooperator(CooperatorConfiguration const& config)
: m_lastRdtsc(0)
, m_ticks(0)
, m_config(config)
, m_shutdown(false)
, m_uring(config.uring)
, m_scheduled(nullptr)
, m_name{}
, m_submitFd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
, m_epochMgr(this)
{
    assert(m_submitFd >= 0);
    memcpy(m_name, config.name, sizeof(m_name));

    auto& registry = detail::CooperatorVarRegistry::Instance();
    assert(registry.TotalSize() <= LOCAL_STORAGE_SIZE
           && "CooperatorVar registrations exceed LOCAL_STORAGE_SIZE");
    registry.ConstructAll(m_localStorage);
}

Cooperator::~Cooperator()
{
    // Destruct cooperator-vars before cooperator's own members. This ensures var destructors
    // can safely reference cooperator infrastructure (epoch manager, uring, etc.).
    //
    detail::CooperatorVarRegistry::Instance().DestructAll(m_localStorage);

    if (m_submitFd >= 0)
    {
        close(m_submitFd);
    }

    std::lock_guard<std::mutex> lock(s_registryMutex);
    if (!Disconnected())
    {
        s_registry.Remove(this);
    }
}

void Cooperator::Shutdown()
{
    m_shutdown.store(true, detail::kStoreFlag);
    WakeCooperator();
}

void Cooperator::ShutdownAll()
{
    // Close the gate so no new cooperators can register, then take the lock. Any Launch() that
    // beat us to the lock will have registered and be visible in the list; any Launch() that
    // follows will see the flag under the lock and shut itself down.
    //
    s_registryShutdown.store(true, detail::kStoreFlag);

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
    s_registryShutdown.store(false, detail::kStoreFlag);
}

bool Cooperator::Submit(
    void(*func)(Context*, void*),
    void* arg,
    SpawnConfiguration const& config /* = s_defaultConfiguration */)
{
    return Submit([func, arg](Context* ctx) { func(ctx, arg); }, config);
}

void Cooperator::PushSubmission(SubmissionEntry* entry)
{
    std::lock_guard<std::mutex> lock(m_submissionLock);
    entry->m_next = nullptr;
    if (m_submissionTail)
    {
        m_submissionTail->m_next = entry;
    }
    else
    {
        m_submissionHead = entry;
    }
    m_submissionTail = entry;
    m_hasSubmissions.store(true, std::memory_order_release);
}

void Cooperator::WakeCooperator()
{
    uint64_t val = 1;
    [[maybe_unused]] auto ret = write(m_submitFd, &val, sizeof(val));
}

void Cooperator::DrainSubmissions()
{
    if (!m_hasSubmissions.load(std::memory_order_acquire))
    {
        return;
    }

    SubmissionEntry* head;
    {
        std::lock_guard<std::mutex> lock(m_submissionLock);
        head = m_submissionHead;
        m_submissionHead = nullptr;
        m_submissionTail = nullptr;
        m_hasSubmissions.store(false, std::memory_order_relaxed);
    }

    while (head)
    {
        auto* entry = head;
        head = head->m_next;
        SpawnFromSubmission(entry);
    }
}


void Cooperator::SpawnFromSubmission(SubmissionEntry* entry)
{
    auto* completion = entry->m_completion;
    auto* completionOk = entry->m_completionOk;
    bool spawned = Spawn(entry->m_config, [entry, completion, completionOk](Context* ctx)
    {
        entry->m_invoke(entry, ctx);
        entry->m_destroy(entry);
        if (completionOk)
        {
            *completionOk = true;
        }
        if (completion)
        {
            completion->release();
        }
    });

    if (!spawned)
    {
        // Spawn can fail during shutdown or allocation pressure; complete the submission
        // bookkeeping here so SubmitSync callers do not block forever.
        if (completionOk)
        {
            *completionOk = false;
        }
        if (completion)
        {
            completion->release();
        }
        entry->m_destroy(entry);
    }
}

void Cooperator::DrainRemainingSubmissions()
{
    std::lock_guard<std::mutex> lock(m_submissionLock);
    auto* head = m_submissionHead;
    m_submissionHead = nullptr;
    m_submissionTail = nullptr;
    while (head)
    {
        auto* entry = head;
        head = head->m_next;
        if (entry->m_completionOk)
        {
            *entry->m_completionOk = false;
        }
        if (entry->m_completion)
        {
            entry->m_completion->release();
        }
        entry->m_destroy(entry);
    }
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
            COOP_PERF_INC(m_perf, perf::Counter::ContextExit);
            m_stackPool.Free(m_scheduled, m_scheduled->m_segment.Size());
            break;
        }
        case SchedulerJumpResult::YIELDED:
        {
            COOP_PERF_INC(m_perf, perf::Counter::ContextYield);
            m_scheduled->m_state = SchedulerState::YIELDED;
            m_yielded.Push(m_scheduled);
            break;
        }
        case SchedulerJumpResult::BLOCKED:
        {
            COOP_PERF_INC(m_perf, perf::Counter::ContextBlock);
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
        if (s_registryShutdown.load(detail::kLoadFlag))
        {
            Shutdown();
            return;
        }
        s_registry.Push(this);
    }

    Cooperator::thread_cooperator = this;
    epoch::SetManager(&m_epochMgr);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr);

    // Pin this thread to a CPU core. If the config specifies a core, use it; otherwise
    // auto-assign round-robin across available cores. COOP_NO_PIN=1 disables pinning.
    //
    if (!PinningDisabled())
    {
        int cpu = m_config.cpuAffinity;
        if (cpu < 0)
        {
            cpu = NextRoundRobinCpu();
        }
        if (cpu >= 0 && PinThread(cpu) == 0)
        {
            m_cpuId = cpu;
            m_numaNode = GetTopology().NumaNodeForCpu(cpu);
        }
    }

    m_lastRdtsc = rdtsc();

    m_uring.Init();

    // Spawn a detached context that reads the eventfd and drains cross-thread submissions.
    // The eventfd is just another fd with a normal io_uring read — no special-case CQE handling
    // in Poll(). The context owns the fd via io::Descriptor.
    //
    Spawn([this](Context* ctx)
    {
        ctx->SetName("SubmissionDrainer");

        io::Descriptor desc(m_submitFd);

        while (!ctx->IsKilled())
        {
            uint64_t val;
            io::Read(desc, &val, sizeof(val));
            if (ctx->IsKilled()) break;
            DrainSubmissions();
        }

        // Prevent double-close — the cooperator destructor owns the fd.
        //
        desc.m_fd = -1;
    });

    // Check COOP_PERF=1 env var to enable dynamic perf probes at startup (mode 2 only).
    // Static local ensures this runs once even with multiple cooperators.
    //
    static bool envChecked = []{
        const char* perfEnv = getenv("COOP_PERF");
        if (perfEnv && perfEnv[0] == '1') perf::Enable();

        const char* sampleEnv = getenv("COOP_SAMPLE");
        if (sampleEnv)
        {
            int hz = atoi(sampleEnv);
            if (hz > 0) perf::StartSampling(hz);
        }
        return true;
    }();
    (void)envChecked;

    bool shutdownKillDone = false;

    while (!m_yielded.IsEmpty() || !m_shutdown.load(detail::kLoadFlag)
                                 || !shutdownKillDone
                                 || m_uring.PendingOps() > 0)
    {
        // When shutdown is requested, kill all live contexts from within the cooperator's
        // thread so they can exit naturally. This only needs to happen once; killed contexts
        // will check IsKilled() when resumed and return.
        //
        if (m_shutdown.load(detail::kLoadFlag) && !shutdownKillDone)
        {
            shutdownKillDone = true;
            DrainSubmissions();
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

            // Wake the eventfd so the submission drainer's io::Read completes. The drainer
            // uses non-kill-aware IO; it checks IsKilled() after each read returns.
            //
            WakeCooperator();
        }

        if (m_yielded.IsEmpty())
        {
            m_uring.Poll();

            if (!m_yielded.IsEmpty())
            {
                continue;
            }

            // Blocked contexts or pending IO — keep spinning for completions.
            //
            if (!m_blocked.IsEmpty() || m_uring.PendingOps() > 0)
            {
                continue;
            }

            if (m_shutdown.load(detail::kLoadFlag))
            {
                continue;
            }

            // Truly idle — block until a CQE arrives. The submission drainer's pending
            // io::Read on the eventfd will wake this when a cross-thread Submit arrives.
            //
            m_uring.WaitAndPoll();
            continue;
        }

        int remainingIterations = 16;
        while (remainingIterations && !m_yielded.IsEmpty())
        {
            COOP_PERF_INC(m_perf, perf::Counter::SchedulerLoop);
            remainingIterations--;

            auto* ctx = m_yielded.Pop();
            Resume(ctx);
            m_uring.Poll();
        }
    }

    epoch::SetManager(nullptr);

    // Signal any SubmitSync callers that arrived after the loop exited
    //
    DrainRemainingSubmissions();

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

    COOP_PERF_INC(m_perf, perf::Counter::ContextResume);
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

    // In MultiCoordinator scenarios, two coordinators can fire before the consumer runs its
    // cleanup pass. The first Release already moved the context out of BLOCKED; the second
    // arrives here with the context in YIELDED or RUNNING. Silently ignore — the
    // MultiCoordinator cleanup will Release the extra coordinator when the consumer runs.
    //
    if (ctx->m_state != SchedulerState::BLOCKED)
    {
        return;
    }

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

    // Destruct ContextVar slots. These run after the task and its Launchable are gone but before
    // ~Context(), so the context is still alive for any non-cooperative teardown. ContextVar
    // destructors must not yield or block.
    //
    coop::detail::ContextVarRegistry::Instance().DestructAll(ctx->m_segment.Bottom());

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
    COOP_PERF_INC(m_perf, perf::Counter::ContextSpawn);
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

// BoundarySafeKill is invoked by Handles, which can be called within the cooperator's thread or
// from outside of it (boundary crossing).
//
void Cooperator::BoundarySafeKill(Context::Handle* handle, const bool crossed /* = false */)
{
    if (this != Cooperator::thread_cooperator)
    {
        assert(!crossed);

        // Cross the thread boundary: submit work onto the cooperator and block until it
        // completes. SubmitSync guarantees the kill logic runs before we return.
        //
        if (!SubmitSync([handle](Context* ctx)
        {
            ctx->GetCooperator()->BoundarySafeKill(handle, true /* crossed */);
        }))
        {
            // Cooperator is shutting down; the kill will be handled by the shutdown sweep
            //
            return;
        }
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
        printf("%s (%p) [%s%s] {yields=%lu, blocks=%lu, ticks=%lu, io=%lu/%lu}\n",
            ctx->GetName(),
            ctx,
            status,
            ctx->IsKilled() ? "!" : "",
            ctx->m_statistics.yields,
            ctx->m_statistics.blocks,
            ctx->m_statistics.ticks,
            ctx->m_statistics.ioSubmits,
            ctx->m_statistics.ioCompletes);
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
#if defined(__x86_64__)
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
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return static_cast<int64_t>(val);
#else
#error "Unsupported architecture for rdtsc"
#endif
}

thread_local Cooperator* Cooperator::thread_cooperator;

} // end namespace coop
