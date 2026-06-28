#pragma once

#include <atomic>
#include <cassert>
#include <linux/time_types.h>
#include <mutex>
#include <semaphore>

#include "detail/embedded_list.h"
#include "detail/memory_order.h"
#include "context.h"
#include "continuation_pool.h"
#include "coordinator.h"
#include "epoch/epoch.h"
#include "cooperator_configuration.h"
#include "cooperator_var.h"
#include "spawn_configuration.h"
#include "stack_pool.h"
#include "perf/counters.h"
#include "io/uring.h"
#include "time/timer_queue.h"
#include "topology.h"

extern "C" void CoopContextEntry(coop::Context* ctx);

namespace coop
{

enum class SchedulerJumpResult
{
    DEFAULT = 0,
    EXITED,
    YIELDED,
    BLOCKED,
    RESUMED,
};

struct Launchable;
struct CooperateHandle;

namespace work { struct Participation; }

static constexpr int COOPERATOR_LIST_REGISTRY = 0;

// A Cooperator manages multiple contexts
//
struct Cooperator : EmbeddedListHookups<Cooperator, int, COOPERATOR_LIST_REGISTRY>
{
    using RegistryList = EmbeddedList<Cooperator, int, COOPERATOR_LIST_REGISTRY>;
    thread_local static Cooperator* thread_cooperator;

    static constexpr size_t LOCAL_STORAGE_SIZE = 4096;

    Cooperator(CooperatorConfiguration const& config = s_defaultCooperatorConfiguration);
    ~Cooperator();

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
    bool Spawn(Fn const& fn, Context::Handle* handle = nullptr);

    template<typename Fn>
    bool Spawn(
        SpawnConfiguration const& config,
        Fn const& fn,
        Context::Handle* handle = nullptr);

    // Launch is an alternative, in-context API where Launchable types can be constructed and
    // given their own context to execute in.
    //
    // This is a slightly dangerous API in two ways:
    //  (1) The context that is passed into the constructor is not the current context
    //  (2) The returned instance may already be destructed.
    //
    // The former is unfortunate and would be nice to patch up. The latter is just the nature of
    // things; if you know enough about the type to want to touch it after, then you should also
    // know enough to know if it is safe.
    //
    template<typename T, typename... Args>
    T* Launch(SpawnConfiguration const&, Context::Handle*, Args&&...);

    template<typename T, typename... Args>
    T* Launch(SpawnConfiguration const& config, Args&&... args)
    {
        Context::Handle* h = nullptr;
        return Launch<T>(config, h, std::forward<Args&&>(args)...);
    }

    template<typename T, typename... Args>
    T* Launch(Context::Handle* h, Args&&... args)
    {
        return Launch<T>(s_defaultConfiguration, h, std::forward<Args&&>(args)...);
    }

    template<typename T, typename... Args>
    T* Launch(Args&&... args)
    {
        Context::Handle* h = nullptr;
        return Launch<T>(s_defaultConfiguration, h, std::forward<Args&&>(args)...);
    }

    // Cooperate dispatches work from a cooperative context onto this (possibly different)
    // cooperator. If a CooperateHandle is provided, the handle's signal fires once the target
    // cooperator has attempted to spawn the work — check handle->m_spawnOk for success.
    // The handle does NOT signal work completion; that is the caller's responsibility.
    //
    // Returns false immediately if the target is shutting down (handle is NOT signaled in
    // this case). Returns true if the submission was enqueued (handle will be signaled).
    //
    // Must be called from a cooperative context (assert on thread_cooperator).
    // For external-thread dispatch, use Submit.
    //
    template<typename Fn>
    bool Cooperate(Fn&& fn, CooperateHandle* handle = nullptr,
                   SpawnConfiguration const& config = s_defaultConfiguration);

    // Submit queues work from an external thread onto this cooperator. The cooperator wakes via
    // eventfd and spawns a context to execute the lambda. Fire-and-forget — the lambda is
    // heap-allocated and freed after execution. Returns false if the cooperator is shutting down.
    //
    template<typename Fn>
    bool Submit(Fn&& fn, SpawnConfiguration const& config = s_defaultConfiguration);

    // SubmitSync queues work and blocks the calling thread until the spawned context completes.
    // The caller's lambda lifetime is guaranteed (caller is blocked). Returns false if the
    // cooperator is shutting down before enqueue, or if queued work cannot be spawned and run.
    //
    template<typename Fn>
    bool SubmitSync(Fn&& fn, SpawnConfiguration const& config = s_defaultConfiguration);

    // Legacy Submit with pthread_create-style function pointer + void* arg. Wraps to the
    // template Submit internally.
    //
    bool Submit(
        void(*func)(Context*, void*),
        void* arg = nullptr,
        SpawnConfiguration const& config = s_defaultConfiguration);

    // Internal API for passing control to the given context
    //
    void Resume(Context* ctx);

    // The other half of `Yield()` which resumes into the cooperator's code
    //
    void YieldFrom(Context* ctx);

    void Block(Context* ctx);

    void Unblock(Context* ctx, const bool schedule);

    // Shared wake dispatch for a Coordinated waiter, used by every site that wakes one
    // (Coordinator::Release, Signal::Notify): a continuation waiter is queued to fire from the
    // loop's drain (DrainContinuationsPaced); a context waiter is Unblock'd. Routing all wakes here is
    // what makes continuations work with any coordinator-backed primitive, not just one.
    //
    void WakeWaiter(Coordinated* waiter, const bool schedule);

    // Run *all* queued continuations to completion as function calls (no context switch), leaving
    // the pending list empty on return. Iterative, so a continuation that queues another is
    // flattened rather than recursing on the native stack.
    //
    // This is the unconditional contract a caller can rely on: once it returns, no queued
    // continuation survives to fire later. That matters because a detached continuation captures
    // state owned by its registrar (a coordinator, a counter, a stop flag); if the registrar tears
    // that state down believing the queue is drained, a continuation fired afterward reads freed
    // memory. The scheduler loop does NOT call this -- it paces its drain with
    // DrainContinuationsPaced so an always-ready synchronous chain cannot starve io_uring pickup --
    // but every other caller wants the whole queue gone, not a budget-sized prefix of it.
    //
    void DrainContinuations();

    // Scheduler-internal, latency-paced drain: fire at most m_drainBudget continuations, peeking
    // io_uring every m_drainPeekStride to break the instant real completions are pending. Whatever
    // is left rides the next loop iteration, which Polls first -- the bound that turns drain length
    // into bounded completion-pickup latency (see the m_drainBudget / m_drainPeekStride comments).
    //
    // Only the scheduler loop may leave the queue partially drained, because only the scheduler
    // guarantees it will come back and finish it before anything observes the gap. A general caller
    // must use DrainContinuations (full drain); a budget-truncated public drain silently strands
    // continuations, which is the bug this split exists to prevent.
    //
    void DrainContinuationsPaced();

    // Backing store for detached continuations (Coordinator::ContinueDetached). Allocated, fired,
    // and freed on this cooperator's thread, so the pool needs no synchronization. Routed through
    // DetachedContinuationImpl's operator new/delete.
    //
    void* AllocateContinuation(size_t n)
    {
        return m_continuationPool.Allocate(n);
    }

    void FreeContinuation(void* p, size_t n)
    {
        m_continuationPool.Free(p, n);
    }

#ifndef NDEBUG
    // Debug-only: set (via detail::ThunkScope) while a Thunk -- a Continuation or an Erg -- runs, so
    // that suspending operations can assert at the misuse site. Compiled out in release. See thunk.h.
    //
    bool m_inThunk = false;
#endif

    // Opt-in work-sharing participation: null unless this cooperator has joined a work::Grid. Shed()
    // reads it -- with a Grid it sheds a balanced Erg, without one it falls back to Spawn (the
    // "shed = spawn" default). Set by work::Grid::Join; never touched on the scheduler hot path.
    //
    work::Participation* m_participation = nullptr;

    // Continuation-drain pacing. The drain otherwise runs the pending list strictly to empty before
    // the scheduler loop returns to its top -- and therefore to the next io_uring Poll. That is
    // ideal for the usual IO-paced chain (each stage waits on a CQE, so the list empties naturally)
    // and it buys bounded native stack depth and I-cache locality for free. The hazard is a chain
    // that re-arms and fires synchronously -- a pipeline stage whose next stage is always ready
    // (buffered data, an already-released coordinator) rather than waiting on a CQE. Such a chain
    // monopolizes the drain and delays the next Poll, inflating the completion-pickup tail latency
    // of unrelated IO on this cooperator: the in-cooperator analogue of reactor starvation.
    //
    // Two cooperating bounds tame it. m_drainPeekStride is the primary, precise control: every that
    // many fired continuations the drain peeks io_uring (a userspace ring read, no syscall) and
    // breaks the instant real completions are pending, so a CQE sitting behind the chain is picked
    // up within a stride rather than waiting out a fixed count. When no IO is pending the peek costs
    // nothing observable and the chain runs on -- a pure synchronous chain pays no spurious breaks.
    // m_drainBudget is the coarse backstop: a hard ceiling that bounds native stack depth and caps a
    // pathological chain that somehow never lets a completion land. The budget alone is a blunt proxy
    // for "is IO waiting?" -- too low wastes Polls on a quiet chain, too high lets a CQE languish; the
    // peek supplies the real signal and the budget just guarantees an upper bound.
    //
    // When the drain breaks with continuations still queued they are runnable now, so the loop's idle
    // branch loops back to Poll (harvesting whatever CQE arrived) and re-drains the remainder -- the
    // chain still makes full progress, interleaved with completion pickup. Stride 0 disables the peek
    // (budget-only); budget 0 disables the ceiling (peek-only); both 0 drains strictly to empty.
    // Single-cooperator state -- read and written only on this thread, no synchronization.
    //
    uint32_t m_drainPeekStride = kDefaultDrainPeekStride;
    uint32_t m_drainBudget = kDefaultDrainBudget;

    static constexpr uint32_t kDefaultDrainPeekStride = 16;
    static constexpr uint32_t kDefaultDrainBudget = 256;

    void Shutdown();

    static void ShutdownAll();

    // Reset the global shutdown state so that new cooperators can register again. All cooperators
    // must have fully exited before this is called. This exists for test harnesses that need to
    // exercise ShutdownAll() without poisoning the global state for subsequent tests.
    //
    static void ResetGlobalShutdown();

    bool IsShuttingDown() const
    {
        return m_shutdown.load(detail::kLoadFlag);
    }

    io::Uring* GetUring()
    {
        return &m_uring;
    }

    // Per-cooperator timer queue (docs/timer_wheel_001.md). A Sleeper registers its absolute
    // deadline and the coordinator to Release on expiry, instead of arming its own kernel timer; the
    // scheduler services the queue and keeps a single IORING_OP_TIMEOUT armed for the nearest
    // deadline. Both run on this cooperator's thread only — no synchronization.
    //
    void RegisterTimer(time::TimerNode* node, int64_t deadlineUs, Coordinator* coord)
    {
        m_timers.Insert(node, deadlineUs, coord);
    }

    void CancelTimer(time::TimerNode* node)
    {
        m_timers.Remove(node);
    }

    // Invoked from io::Handle::Callback when the single per-cooperator deadline timer's CQE arrives.
    // The expiry itself is serviced by the scheduler loop (ServiceExpiredTimers); this only clears
    // the armed flag so the loop re-arms for the next nearest deadline.
    //
    void OnTimerExpired() { m_timerArmed = false; }

    // Whether pure-timer deadlines on this cooperator use the userspace queue (one kernel timer for
    // the nearest deadline) or the default kernel-per-timer path. Selected by
    // CooperatorConfiguration::timerMode. Read by Sleeper to choose its backing.
    //
    bool UsesTimerQueue() const { return m_config.timerMode == TimerMode::UserspaceQueue; }

    int CpuId() const { return m_cpuId; }
    int NumaNode() const { return m_numaNode; }

    perf::Counters& GetPerfCounters() { return m_perf; }

    epoch::Manager& GetEpochManager() { return m_epochMgr; }

    // Read the epoch watermark. Safe to call cross-thread (atomic load).
    //
    epoch::Epoch GetEpochWatermark() const
    {
        return m_epochWatermark.load(std::memory_order_acquire);
    }

    const char* GetName() const { return m_name; }

    // Visit all registered cooperators under the registry lock. The callback receives each
    // cooperator pointer. Return false from the callback to stop iteration early.
    //
    template<typename Fn>
    static void VisitRegistry(Fn const& fn)
    {
        std::lock_guard<std::mutex> lock(s_registryMutex);
        s_registry.Visit(fn);
    }

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

    template<typename Fn>
    void VisitContexts(Fn const& fn) { m_contexts.Visit(fn); }

    int64_t GetTicks() const { return m_ticks; }

    void SanityCheck();

    // Ideally this would be better protected
    //
    void BoundarySafeKill(Context::Handle*, const bool crossed = false);

    void PrintContextTree(Context* ctx = nullptr, int indent = 0) ;

    // Type-erased submission entry for cross-thread work dispatch. Public because TypedSubmission
    // (in cooperator.hpp) inherits from it.
    //
    struct SubmissionEntry
    {
        SubmissionEntry* m_next{nullptr};
        void (*m_invoke)(SubmissionEntry*, Context*);
        void (*m_destroy)(SubmissionEntry*);
        SpawnConfiguration m_config;
        std::binary_semaphore* m_completion{nullptr};
        bool* m_completionOk{nullptr};
    };

  private:
    // Shared logic for entering a newly created context. Handles saving the spawning context's
    // state, setting up the new stack via ContextInit, and switching to it. Used by both Spawn
    // and Launch to eliminate duplication.
    //
    void EnterContext(Context* ctx);

    void HandleCooperatorResumption(const SchedulerJumpResult res);

    // Timer-queue servicing and the single kernel timer. ServiceExpiredTimers releases every sleep
    // whose deadline has passed; ArmNearestTimer keeps one IORING_OP_TIMEOUT armed (or rescheduled
    // via IORING_TIMEOUT_UPDATE) for the nearest deadline before the loop blocks. See
    // docs/timer_wheel_001.md.
    //
    void ServiceExpiredTimers();
    void ArmNearestTimer();
    void ArmTimerKernel(int64_t deadlineUs, bool update);

    int64_t rdtsc() const;

    int64_t m_lastRdtsc;
    int64_t m_ticks;

    int m_cpuId{-1};
    int m_numaNode{-1};
    CooperatorConfiguration m_config;

    std::atomic<bool> m_shutdown;
    Context*        m_scheduled;

    io::Uring       m_uring;

    // Deadline-ordered queue of in-flight sleeps and the bookkeeping for the one kernel timer that
    // backs them. m_timerArmed says whether an IORING_OP_TIMEOUT is in flight; m_timerDeadlineUs is
    // the absolute deadline it is set to; m_timerTs backs the in-flight SQE's timespec (the kernel
    // copies it at submit, so a single member suffices). All cooperator-thread-local.
    //
    time::TimerQueue m_timers;
    bool             m_timerArmed{false};
    int64_t          m_timerDeadlineUs{0};
    struct __kernel_timespec m_timerTs{};

    StackPool       m_stackPool;
    perf::Counters  m_perf;
    char            m_name[COOPERATOR_NAME_MAX];

    // Cache-line partitioning of the Cooperator's hottest fields. m_sp is written on every
    // context switch (the scheduler saves its own stack pointer here before resuming a context).
    // The submission quartet below is dirtied by external producer threads on every cross-thread
    // Submit, and m_epochWatermark is read cross-thread by peers computing the reclamation
    // horizon. Left adjacent, a producer's Submit (or a peer's epoch-horizon read) would
    // invalidate the very line the owner needs for its next context switch — false sharing on the
    // single hottest write in the runtime. The alignas(64) markers isolate three distinct access
    // groups onto their own lines: owner-hot (m_sp), cross-thread-inbound (the submission
    // quartet), and cross-thread-published (m_epochWatermark). The effect grows on weak-memory
    // architectures (aarch64), where remote-line acquisition is costlier than on x86 TSO.
    //
    alignas(64) void*       m_sp{nullptr};

    void PushSubmission(SubmissionEntry* entry);
    void WakeCooperator();
    void DrainSubmissions();
    void SpawnFromSubmission(SubmissionEntry* entry);
    void DrainRemainingSubmissions();

    alignas(64) int         m_submitFd;
    std::mutex              m_submissionLock;
    SubmissionEntry*        m_submissionHead{nullptr};
    SubmissionEntry*        m_submissionTail{nullptr};
    std::atomic<bool>                   m_hasSubmissions{false};

    // Minimum pinned epoch across all contexts on this cooperator. Written by the cooperator
    // thread (via epoch::Manager::PublishWatermark) after each pin/unpin. Read cross-thread by
    // epoch::Manager::SafeEpoch() on other cooperators to compute the global reclamation horizon.
    // On its own line so peers' horizon reads do not ping-pong the submission quartet.
    //
    alignas(64) std::atomic<epoch::Epoch> m_epochWatermark{epoch::Epoch::Alive()};

    // Per-cooperator epoch manager. Constructed with this* so it can be a member even before
    // thread_cooperator is set. Declared after m_epochWatermark so its destructor (which resets
    // the watermark) runs while m_epochWatermark is still valid.
    //
    epoch::Manager                      m_epochMgr;

    // TODO this is a super dirty thing where the context removes itself from the
    // contexts list when it destructs.
    //
    // I don't think this is actually necessary though because Spawn/Launch should always be the
    // place the destruction happens, strictly speaking, but the context-destructing gets
    // super complex as it is right now.
    //
    friend struct Context;
    friend struct epoch::Manager;
    friend void ::CoopContextEntry(::coop::Context*);

    template<typename T>
    friend struct CooperatorVar;

    Context::AllContextsList    m_contexts;
    Context::ContextStateList   m_yielded;
    Context::ContextStateList   m_blocked;
    Coordinated::List           m_pendingContinuations;
    ContinuationPool            m_continuationPool;
    Context::ContextStateList   m_zombie;

    // Per-cooperator extensible storage for higher layers without
    // coupling the scheduler to those subsystems. Offsets are assigned at static init via
    // CooperatorVarRegistry; access is thread-local cooperator + constant offset.
    //
    alignas(64) char m_localStorage[LOCAL_STORAGE_SIZE];

    static std::atomic<bool> s_registryShutdown;
    static std::mutex        s_registryMutex;
    static RegistryList      s_registry;
};

} // end namespace coop

#include "cooperator.hpp"
