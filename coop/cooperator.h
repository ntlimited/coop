#pragma once

#include <atomic>
#include <cassert>
#include <mutex>
#include <semaphore>

#include "detail/embedded_list.h"
#include "context.h"
#include "epoch/epoch.h"
#include "cooperator_configuration.h"
#include "spawn_configuration.h"
#include "stack_pool.h"
#include "perf/counters.h"
#include "io/uring.h"
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

static constexpr int COOPERATOR_LIST_REGISTRY = 0;

// A Cooperator manages multiple contexts
//
struct Cooperator : EmbeddedListHookups<Cooperator, int, COOPERATOR_LIST_REGISTRY>
{
    using RegistryList = EmbeddedList<Cooperator, int, COOPERATOR_LIST_REGISTRY>;
    thread_local static Cooperator* thread_cooperator;

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

    // Submit queues work from an external thread onto this cooperator. The cooperator wakes via
    // eventfd and spawns a context to execute the lambda. Fire-and-forget — the lambda is
    // heap-allocated and freed after execution. Returns false if the cooperator is shutting down.
    //
    template<typename Fn>
    bool Submit(Fn&& fn, SpawnConfiguration const& config = s_defaultConfiguration);

    // SubmitSync queues work and blocks the calling thread until the spawned context completes.
    // The caller's lambda lifetime is guaranteed (caller is blocked). Returns false without
    // blocking if the cooperator is shutting down.
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

    void Shutdown();

    static void ShutdownAll();

    // Reset the global shutdown state so that new cooperators can register again. All cooperators
    // must have fully exited before this is called. This exists for test harnesses that need to
    // exercise ShutdownAll() without poisoning the global state for subsequent tests.
    //
    static void ResetGlobalShutdown();

    bool IsShuttingDown() const
    {
        return m_shutdown.load(std::memory_order_relaxed);
    }

    io::Uring* GetUring()
    {
        return &m_uring;
    }

    int CpuId() const { return m_cpuId; }
    int NumaNode() const { return m_numaNode; }

    perf::Counters& GetPerfCounters() { return m_perf; }

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
    };

  private:
    // Shared logic for entering a newly created context. Handles saving the spawning context's
    // state, setting up the new stack via ContextInit, and switching to it. Used by both Spawn
    // and Launch to eliminate duplication.
    //
    void EnterContext(Context* ctx);

    void HandleCooperatorResumption(const SchedulerJumpResult res);

    int64_t rdtsc() const;

    int64_t m_lastRdtsc;
    int64_t m_ticks;

    int m_cpuId{-1};
    int m_numaNode{-1};
    CooperatorConfiguration m_config;

    std::atomic<bool> m_shutdown;
    Context*        m_scheduled;

    io::Uring       m_uring;
    StackPool       m_stackPool;
    perf::Counters  m_perf;
    char            m_name[COOPERATOR_NAME_MAX];

    void*           m_sp{nullptr};

    void PushSubmission(SubmissionEntry* entry);
    void WakeCooperator();
    void DrainSubmissions();
    void SpawnFromSubmission(SubmissionEntry* entry);
    void DrainRemainingSubmissions();

    int                     m_submitFd;
    std::mutex              m_submissionLock;
    SubmissionEntry*        m_submissionHead{nullptr};
    SubmissionEntry*        m_submissionTail{nullptr};
    std::atomic<bool>                   m_hasSubmissions{false};

    // Minimum pinned epoch across all contexts on this cooperator. Written by the cooperator
    // thread (via epoch::Manager::PublishWatermark) after each pin/unpin. Read cross-thread by
    // epoch::Manager::SafeEpoch() on other cooperators to compute the global reclamation horizon.
    //
    std::atomic<epoch::Epoch>           m_epochWatermark{epoch::Epoch::Alive()};

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
    Context::AllContextsList    m_contexts;
    Context::ContextStateList   m_yielded;
    Context::ContextStateList   m_blocked;
    Context::ContextStateList   m_zombie;

    static std::atomic<bool> s_registryShutdown;
    static std::mutex        s_registryMutex;
    static RegistryList      s_registry;
};

} // end namespace coop

#include "cooperator.hpp"
