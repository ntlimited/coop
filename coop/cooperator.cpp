#include <functional>
#include <liburing.h>
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
#include "cooperate.h"
#include "context_var.h"
#include "detail/context_switch.h"
#include "detail/memory_order.h"
#include "io/descriptor.h"
#include "io/read.h"
#include "launchable.h"
#include "perf/patch.h"
#include "perf/probe.h"
#include "perf/sampler.h"
#include "detail/timer_tag.h"
#include "time/now.h"

namespace coop
{

#ifndef NDEBUG
namespace detail
{
// The current cooperator's "running inside a Thunk" flag (see thunk.h). Single-threaded within a
// cooperator, so a plain bool needs no synchronization; null-safe so RunErg etc. are callable from
// a raw (non-cooperator) thread without tracking.
//
bool EnterThunk()
{
    auto* co = Cooperator::thread_cooperator;
    if (!co) return false;
    const bool prev = co->m_inThunk;
    co->m_inThunk = true;
    return prev;
}
void ExitThunk(bool prev)
{
    if (auto* co = Cooperator::thread_cooperator) co->m_inThunk = prev;
}
bool InThunk()
{
    auto* co = Cooperator::thread_cooperator;
    return co && co->m_inThunk;
}
}
#endif

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
    bool isCooperate = (reinterpret_cast<uintptr_t>(entry->m_completion) & 1) != 0;

    if (isCooperate)
    {
        auto* handle = reinterpret_cast<CooperateHandle*>(
            reinterpret_cast<uintptr_t>(entry->m_completion) & ~uintptr_t(1));
        auto* callerCoop = reinterpret_cast<Cooperator*>(entry->m_completionOk);

        bool spawned = Spawn(entry->m_config, [entry](Context* ctx)
        {
            entry->m_invoke(entry, ctx);
            entry->m_destroy(entry);
        });
        if (!spawned)
            entry->m_destroy(entry);

        if (handle && callerCoop)
        {
            handle->m_spawnOk = spawned;

            // Push completion notification back to caller's cooperator so Signal::Notify
            // runs in the caller's thread (Signal is local to that cooperator).
            //
            auto* notifEntry = new TypedSubmission<std::function<void(Context*)>>(
                [handle](Context* ctx)
                {
                    handle->m_signal.Notify(ctx, false);
                }, s_defaultConfiguration);
            callerCoop->PushSubmission(notifEntry);
            callerCoop->WakeCooperator();
        }
        return;
    }

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

        bool isCooperate = (reinterpret_cast<uintptr_t>(entry->m_completion) & 1) != 0;
        if (isCooperate)
        {
            auto* handle = reinterpret_cast<CooperateHandle*>(
                reinterpret_cast<uintptr_t>(entry->m_completion) & ~uintptr_t(1));
            auto* callerCoop = reinterpret_cast<Cooperator*>(entry->m_completionOk);

            if (handle && callerCoop)
            {
                handle->m_spawnOk = false;
                auto* notifEntry = new TypedSubmission<std::function<void(Context*)>>(
                    [handle](Context* ctx)
                    {
                        handle->m_signal.Notify(ctx, false);
                    }, s_defaultConfiguration);
                callerCoop->PushSubmission(notifEntry);
                callerCoop->WakeCooperator();
            }

            entry->m_destroy(entry);
            continue;
        }

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
    // for its own accounting. This single rdtsc serves both purposes. It is gated behind
    // trackContextCycles because the rdtsc is the dominant per-resume cost and the ticks it feeds
    // are observability only (see CooperatorConfiguration::trackContextCycles).
    //
    if (m_config.trackContextCycles)
    {
        auto now = rdtsc();
        m_scheduled->m_statistics.ticks += now - m_scheduled->m_lastRdtsc;
        m_lastRdtsc = now;
    }

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
    // in Poll(). This keeps a CQE-producing wake source in flight even when no user contexts are
    // runnable, so WaitAndPoll() can sleep on submissions instead of spinning.
    //
    Spawn([this](Context* ctx)
    {
        ctx->SetName("SubmissionDrainer");

        io::Descriptor desc(io::borrowed, m_submitFd);

        while (!ctx->IsKilled())
        {
            uint64_t val;
            io::Read(desc, &val, sizeof(val));
            if (ctx->IsKilled()) break;
            DrainSubmissions();
        }
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
                                 || !m_pendingContinuations.IsEmpty()
                                 || m_uring.PendingOps() > 0)
    {
        if (m_hasSubmissions.load(std::memory_order_acquire))
        {
            DrainSubmissions();
        }

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

            // Release any sleeps whose deadline has passed (the timer CQE, if it fired, was just
            // dispatched by Poll, clearing m_timerArmed). Woken sleepers join the yielded list, so
            // the m_yielded check below picks them up before the loop considers blocking.
            //
            ServiceExpiredTimers();

            if (m_hasSubmissions.load(std::memory_order_acquire))
            {
                DrainSubmissions();
            }

            // If this Poll completed IO that woke a context, schedule it before re-draining a
            // continuation backlog. The backlog is the synchronous chain the CQE-peek broke us out
            // of; the freshly-completed IO is exactly the work that was waiting behind it, and
            // re-draining the chain to its budget ceiling first is what would reintroduce the
            // completion-pickup tail the peek is meant to cut. The queued continuations are not
            // lost — they ride the next iteration's drain, interleaved with the woken context.
            //
            if (!m_yielded.IsEmpty())
            {
                continue;
            }

            // No context woke. Fire continuations queued by this poll's CQEs (and any earlier
            // release) before we consider blocking — they are runnable work, and they may wake
            // contexts of their own. Paced: a budget-truncated remainder rides the next iteration,
            // which Polls first (the m_pendingContinuations guard below loops us back).
            //
            DrainContinuationsPaced();

            if (!m_yielded.IsEmpty())
            {
                continue;
            }

            // A budget-truncated drain left continuations queued. They are runnable now, so loop
            // back to the top to Poll (harvesting any CQE that arrived meanwhile, e.g. an unrelated
            // IO completion that was waiting behind the chain) and fire the next budgeted batch,
            // rather than sleeping with runnable work in hand. This is the half of the bound that
            // turns drain length into bounded completion-pickup latency.
            //
            if (!m_pendingContinuations.IsEmpty())
            {
                continue;
            }

            // No runnable contexts. Sleep in io_uring until the next CQE instead of spinning.
            // The submission drainer's eventfd read is an ordinary in-flight uring op, so
            // cross-thread Submit() also wakes this path via CQE delivery.
            //
            if (!m_blocked.IsEmpty() || m_uring.PendingOps() > 0)
            {
                // Ensure one kernel timer is armed for the nearest deadline before sleeping, so the
                // wait wakes when the soonest sleep comes due. WaitAndPoll submits the SQE.
                //
                ArmNearestTimer();
                m_uring.WaitAndPoll();
                continue;
            }

            if (m_shutdown.load(detail::kLoadFlag))
            {
                continue;
            }
        }

        int remainingIterations = 16;
        while (remainingIterations && !m_yielded.IsEmpty())
        {
            COOP_PERF_INC(m_perf, perf::Counter::SchedulerLoop);
            remainingIterations--;

            auto* ctx = m_yielded.Pop();
            Resume(ctx);

            // Was anything already runnable going into this reap? If not, and the reap then wakes a
            // context, that context is a fresh IO completion with nothing ahead of it -- and it
            // should be resumed before the continuation backlog is re-drained, the same
            // woken-context-first ordering the idle branch above uses. Re-draining a full budget of
            // a synchronous chain here is exactly what strands a just-completed IO behind it: the
            // resume-batch twin of the idle-branch completion-pickup tail.
            //
            bool hadRunnable = !m_yielded.IsEmpty();

            // Reap completions without entering the kernel. SQEs armed by this resume but not
            // yet submitted stay deferred and accumulate; the single Poll() at the batch
            // boundary flushes them in one io_uring_enter rather than one per resume.
            //
            // This only batches SQEs that genuinely reach the loop unsubmitted — for example a
            // context that arms several async ops via the Handle API and then yields. The
            // blocking IO path does not benefit here: io::Recv/Send arm an SQE and then call
            // Handle::Wait(), which submits eagerly to catch an inline completion before
            // blocking, so those ops are already in flight by the time control returns. Folding
            // that eager submit into this batch boundary would batch the blocking path too, but
            // it trades away single-flow latency (the lone op that has no batch to amortize
            // into is submitted a scheduler traversal later), so it is intentionally left as a
            // per-op submit in Wait().
            //
            m_uring.ReapOnly();

            if (m_hasSubmissions.load(std::memory_order_acquire))
            {
                DrainSubmissions();
                if (!m_yielded.IsEmpty())
                {
                    break;
                }
            }

            // Fire continuations after this iteration's reap -- but defer that drain for the
            // lone-wake case just described, letting the loop circle back to resume the woken
            // context first. Whenever a backlog already existed the drain proceeds as before, so a
            // continuation chain is never starved by a steady stream of context wakes: the
            // per-iteration drain still fires, bounded by the op budget. A deferred backlog is not
            // dropped -- it rides a later iteration (or the idle branch) once the freshly woken
            // context has had its turn.
            //
            if (hadRunnable || m_yielded.IsEmpty())
            {
                DrainContinuationsPaced();
            }
        }

        // Batch boundary: flush the SQEs accumulated across the resumes above and harvest any
        // completions in one io_uring_enter. This bounds unsubmitted SQEs to a single batch —
        // when the runnable list stays non-empty across batches the outer loop skips its own
        // Poll(), so without this flush armed IO (including Handle-destructor cancels feeding a
        // Flash barrier) could sit unsubmitted indefinitely.
        //
        m_uring.Poll();

        // Service due sleeps on the busy path too. A loop that never goes idle would otherwise only
        // service timers in the idle branch above; here the loop's own iteration rate is the clock,
        // and no kernel timer is needed while there is other work to run.
        //
        ServiceExpiredTimers();
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
    // Direct-yield fastpath: when another context is already runnable, switch straight into it
    // instead of trampolining back through the cooperator loop (which would cost a second switch
    // plus HandleCooperatorResumption). The yielding context takes the place the loop would have
    // given it -- pushed onto the yielded list -- and the next runnable is resumed directly.
    //
    // The budget bounds how many direct switches happen before one falls back through the loop.
    // The loop is the only place io_uring is polled, so the bound is what keeps CQE processing
    // (and the Handle::Flash teardown barriers that depend on it) from starving: at most
    // directYieldBudget switches separate two polls, no matter how tightly contexts yield.
    //
    if (m_config.directYield && m_directYieldsRemaining > 0 && !m_yielded.IsEmpty())
    {
        --m_directYieldsRemaining;

        Context* next = m_yielded.Pop();

        if (m_config.trackContextCycles)
        {
            auto now = rdtsc();
            ctx->m_statistics.ticks += now - ctx->m_lastRdtsc;
            next->m_lastRdtsc = now;
        }

        ctx->m_state = SchedulerState::YIELDED;
        m_yielded.Push(ctx);

        next->m_state = SchedulerState::RUNNING;
        m_scheduled = next;

        auto ret = ContextSwitch(&ctx->m_sp, next->m_sp,
                                 static_cast<int>(SchedulerJumpResult::RESUMED));
        assert(static_cast<SchedulerJumpResult>(ret) == SchedulerJumpResult::RESUMED);
        return;
    }

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

    // A fresh resume from the loop means io_uring was just polled (the loop polls at every batch
    // boundary and before idling), so the direct-yield budget refills here. The chain of direct
    // yields the resumed context may start is then bounded until control returns to the loop.
    //
    m_directYieldsRemaining = m_config.directYieldBudget;

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

    if (m_config.trackContextCycles)
    {
        auto now = rdtsc();
        prev->m_statistics.ticks += now - prev->m_lastRdtsc;
        ctx->m_lastRdtsc = now;
    }

    prev->m_state = SchedulerState::YIELDED;
    m_yielded.Push(prev);

    ctx->m_state = SchedulerState::RUNNING;
    m_scheduled = ctx;

    auto ret = ContextSwitch(&prev->m_sp, ctx->m_sp, static_cast<int>(SchedulerJumpResult::RESUMED));
    assert(static_cast<SchedulerJumpResult>(ret) == SchedulerJumpResult::RESUMED);
}

void Cooperator::WakeWaiter(Coordinated* waiter, const bool schedule)
{
    waiter->Satisfy();

    // A continuation is queued to fire from the loop drain (no context switch, no current
    // context required). A context waiter is unblocked as before.
    //
    if (waiter->IsContinuation())
    {
        m_pendingContinuations.Push(waiter);
    }
    else
    {
        Unblock(waiter->GetContext(), schedule);
    }
}

void Cooperator::DrainContinuations()
{
    // Run the queue to empty. Iterative, not recursive: a continuation whose body wakes another
    // continuation pushes it back onto this queue, which this same loop picks up — so native stack
    // depth stays bounded no matter how deep the continuation chain is.
    //
    // No budget or peek here, unlike the scheduler's paced drain. A caller invoking this directly
    // expects the queue gone on return: it is about to tear down state the queued continuations
    // capture (the coordinator they re-arm on, the counters they touch). Stopping at a budget would
    // strand the remainder to fire later, after that state is freed — a use-after-free. Latency
    // pacing is the scheduler's job (DrainContinuationsPaced), and only the scheduler's, because
    // only it guarantees it will loop back and finish the queue before anything observes the gap.
    //
    while (auto* waiter = m_pendingContinuations.Pop())
    {
        detail::ThunkScope inThunk;                  // debug: forbid suspending inside the Run
        waiter->GetContinuation()->Run();
    }
}

void Cooperator::DrainContinuationsPaced()
{
    // Same iterative drain as DrainContinuations, but bounded so an always-ready synchronous chain
    // cannot monopolize the loop and starve io_uring completion pickup. Two cooperating bounds, see
    // the field comments on m_drainPeekStride / m_drainBudget for the rationale:
    //
    //   - The peek (m_drainPeekStride): every stride continuations, ask io_uring whether completions
    //     are actually pending (a userspace ring read, no syscall). Break the instant they are, so a
    //     waiting CQE is harvested within a stride. A quiet chain never trips it -- no spurious break.
    //   - The budget (m_drainBudget): a hard ceiling that backstops the peek and bounds stack depth.
    //
    // On break the remainder stays queued; the scheduler loop iterates back to Poll and re-drains,
    // so the chain still completes, interleaved with completion pickup. This partial-drain behavior
    // is safe only because the scheduler always comes back to finish it (see DrainContinuations).
    //
    uint32_t budget = m_drainBudget;
    uint32_t stride = m_drainPeekStride;
    uint32_t sinceCheck = 0;
    while (auto* waiter = m_pendingContinuations.Pop())
    {
        {
            detail::ThunkScope inThunk;              // debug: forbid suspending inside the Run
            waiter->GetContinuation()->Run();
        }
        if (budget && --budget == 0)
        {
            break;
        }
        if (stride && ++sinceCheck >= stride)
        {
            sinceCheck = 0;
            if (m_uring.HasPendingCompletions())
            {
                break;
            }
        }
    }
}

void Cooperator::ServiceExpiredTimers()
{
    // Release every sleep whose deadline has passed. schedule=false moves each woken context to the
    // yielded list rather than switching to it mid-loop. Reads the clock once, and only when there
    // is something to service. Each released coordinator's waiter is the blocked sleeping context;
    // the popped node is already unlinked, so the matching Sleeper destructor is a no-op.
    //
    if (m_timers.Empty())
    {
        return;
    }

    const int64_t now = time::MonotonicMicros();
    while (auto* node = m_timers.PopExpired(now))
    {
        node->GetCoordinator()->Release(nullptr, false /* schedule */);
    }
}

void Cooperator::ArmTimerKernel(int64_t deadlineUs, bool update)
{
    // Relative timeout from now to the deadline, clamped non-negative (a past deadline fires
    // immediately). io_uring copies the timespec into the kernel request at submit, so backing it
    // with the single m_timerTs member is safe: the prior armed SQE was already submitted by an
    // earlier WaitAndPoll before this call overwrites it.
    //
    int64_t relUs = deadlineUs - time::MonotonicMicros();
    if (relUs < 0)
    {
        relUs = 0;
    }
    m_timerTs.tv_sec  = relUs / 1000000;
    m_timerTs.tv_nsec = (relUs % 1000000) * 1000;

    auto* sqe = m_uring.GetSqe();
    assert(sqe);
    if (!update)
    {
        // Fresh timeout. Deliberately not counted in Uring::PendingOps(): a lingering far-future
        // timer must not hold the shutdown loop open, and need not, because sleeping contexts in
        // m_blocked already keep the loop alive exactly as long as there are sleeps to service.
        //
        io_uring_prep_timeout(sqe, &m_timerTs, 0, 0);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(detail::kTimerTag));
    }
    else
    {
        // Reschedule the existing timeout in place. The update reaches the kernel before the (later)
        // armed deadline, so it always finds a live timer; its own acknowledgement CQE carries the
        // ack tag and is ignored. The single timeout object still delivers exactly one expiry CQE.
        //
        io_uring_prep_timeout_update(sqe, &m_timerTs, detail::kTimerTag, 0);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(detail::kTimerAckTag));
    }
}

void Cooperator::ArmNearestTimer()
{
    // Maintain the invariant: if the queue is non-empty, an armed kernel timer exists whose deadline
    // is no later than the nearest queued deadline. Arming earlier than needed is a harmless spurious
    // wake that re-arms; arming later would let a sleep fire late, which the invariant forbids.
    //
    if (m_timers.Empty())
    {
        return;
    }

    const int64_t nearest = m_timers.MinDeadlineUs();
    if (!m_timerArmed)
    {
        ArmTimerKernel(nearest, false /* update */);
        m_timerArmed = true;
        m_timerDeadlineUs = nearest;
    }
    else if (nearest < m_timerDeadlineUs)
    {
        ArmTimerKernel(nearest, true /* update */);
        m_timerDeadlineUs = nearest;
    }
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
    int64_t now = m_config.trackContextCycles ? rdtsc() : 0;

    Context* lastCtx = m_scheduled;
    void** save_sp = lastCtx ? &lastCtx->m_sp : &m_sp;
    bool isSelf = !lastCtx;

    if (lastCtx)
    {
        if (m_config.trackContextCycles)
        {
            lastCtx->m_statistics.ticks += now - lastCtx->m_lastRdtsc;
        }
        lastCtx->m_state = SchedulerState::YIELDED;
        m_yielded.Push(lastCtx);
        m_scheduled = nullptr;
    }
    else if (m_config.trackContextCycles)
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
