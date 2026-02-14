#pragma once

#include <cassert>
#include <csetjmp>
#include <mutex>
#include <semaphore>

#include "embedded_list.h"
#include "context.h"
#include "fixed_list.h"
#include "spawn_configuration.h"

namespace coop
{

namespace time
{

    struct Ticker;

} // end namespace coop::time

namespace io
{

    struct Uring;

} // end namespace coop::io

enum class SchedulerJumpResult
{
    DEFAULT = 0,
    EXITED,
    YIELDED,
    BLOCKED,
    RESUMED,
};

struct Launchable;

// A Cooperator manages multiple contexts
//
struct Cooperator
{
    thread_local static Cooperator* thread_cooperator;

    Cooperator()
    : m_lastRdtsc(0)
    , m_ticks(0)
    , m_shutdown(false)
    , m_ticker(nullptr)
    , m_uring(nullptr)
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

    // TODO do this properly and add more nuances around in vs out of cooperator-safe ooepratins.
    //
    void Shutdown()
    {
        m_shutdown = true;
    }

    bool SetTicker(time::Ticker*);

    bool SetUring(io::Uring* ur)
    {
        m_uring = ur;
        return true;
    }

    time::Ticker* GetTicker();

    io::Uring* GetUring();

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
    void BoundarySafeKill(Context::Handle*, const bool crossed = false);
   
    void PrintContextTree(Context* ctx = nullptr, int indent = 0) ;
  
  private:
    // Shared logic for entering a newly created context. Handles saving the spawning context's
    // state, setting up the new stack via ucontext, and switching to it. Used by both Spawn and
    // Launch to eliminate duplication.
    //
    void EnterContext(Context* ctx);

    // Trampoline invoked by makecontext when a new context starts executing.
    //
    static void ContextEntryPoint(int lo, int hi);

    void HandleCooperatorResumption(const SchedulerJumpResult res);
    
    int64_t rdtsc() const;

    int64_t m_lastRdtsc;
    int64_t m_ticks;

    bool            m_shutdown;
    Context*        m_scheduled;

    time::Ticker*   m_ticker;
    Context::Handle m_tickerHandle;
    io::Uring*      m_uring;
    Context::Handle m_uringHandle;

    std::jmp_buf    m_jmpBuf;
    
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
    Context::AllContextsList    m_contexts;
    Context::ContextStateList   m_yielded;
    Context::ContextStateList   m_blocked;
    Context::ContextStateList   m_zombie;
};

void* AllocateContext(SpawnConfiguration const& config);

void LongJump(std::jmp_buf& buf, SchedulerJumpResult result);

} // end namespace coop

#include "cooperator.hpp"
