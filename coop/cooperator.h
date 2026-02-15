#pragma once

#include <atomic>
#include <cassert>
#include <mutex>
#include <semaphore>

#include "detail/embedded_list.h"
#include "context.h"
#include "cooperator_configuration.h"
#include "detail/fixed_list.h"
#include "spawn_configuration.h"
#include "io/uring.h"

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

    using Submission = void(*)(Context*, void*);

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
    bool Submit(
        Submission func,
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

    bool SpawnSubmitted(bool wait = false);

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

    std::atomic<bool> m_shutdown;
    Context*        m_scheduled;

    io::Uring       m_uring;

    void*           m_sp{nullptr};

    struct ExecutionSubmission
    {
        Submission          func;
        void*               arg;
        SpawnConfiguration  config;
    };

    std::counting_semaphore<8>          m_submissionSemaphore;
    std::counting_semaphore<8>          m_submissionAvailabilitySemaphore;
    std::mutex                          m_submissionLock;
    FixedList<ExecutionSubmission, 8>   m_submissions;

    // TODO this is a super dirty thing where the context removes itself from the
    // contexts list when it destructs.
    //
    // I don't think this is actually necessary though because Spawn/Launch should always be the
    // place the destruction happens, strictly speaking, but the context-destructing gets
    // super complex as it is right now.
    //
    friend struct Context;
    friend void ::CoopContextEntry(::coop::Context*);
    Context::AllContextsList    m_contexts;
    Context::ContextStateList   m_yielded;
    Context::ContextStateList   m_blocked;
    Context::ContextStateList   m_zombie;

    static std::atomic<bool> s_registryShutdown;
    static std::mutex        s_registryMutex;
    static RegistryList      s_registry;
};

void* AllocateContext(SpawnConfiguration const& config);

} // end namespace coop

#include "cooperator.hpp"
