#pragma once

#include <cstdint>

#include "coordinator.h"
#include "epoch/epoch.h"
#include "signal.h"
#include "time/interval.h"
#include "detail/embedded_list.h"
#include "detail/scheduler_state.h"
#include "spawn_configuration.h"

namespace coop
{

template<typename T> struct Alloc;
struct AllocBuffer;

// Why a context died. Extensible; None means "not killed".
//
enum class KillCause : uint8_t
{
    None = 0,
    Kill,       // explicit Kill / parent tree kill
    Deadline,   // the context's deadline budget expired
    Drain,      // graceful shutdown draining
};

struct Segment
{
    size_t m_size;
    uint8_t    m_bottom[0] __attribute__((aligned(128)));
    
    size_t Size() const
    {
        return m_size;
    }

    void* Bottom()
    {
        return reinterpret_cast<void*>(&m_bottom[0]);
    }

    void* Top()
    {
        return reinterpret_cast<void*>(&m_bottom[m_size]);
    }
};

struct Coordinator;
struct CoordinatorExtension;
struct Cooperator;

// Three different groups of mutually exclusive lists are kept for contexts:
// - the list of all contexts for a given cooperator
// - the list of all contexts in a given state for a given cooperator
// - the list of all child contexts for a given cooperator
//
static constexpr int CONTEXT_LIST_ALL = 0;
static constexpr int CONTEXT_LIST_STATE = 1;
static constexpr int CONTEXT_LIST_CHILDREN = 2;

// An Context is what code runs "in," in cooperation with a Cooperator. Each
//
struct Context : EmbeddedListHookups<Context, int, CONTEXT_LIST_ALL>
               , EmbeddedListHookups<Context, int, CONTEXT_LIST_STATE>
               , EmbeddedListHookups<Context, int, CONTEXT_LIST_CHILDREN>
{
    // Embedded lists for tracking the set of lists that contexts can never be in more
    // than one of, e.g. there are multiple `ContextStateList`s in the cooperator, but contexts are
    // never in both active and yielded, etc. Or in two different cooperator's active list...
    //
    using AllContextsList = EmbeddedList<Context, int, CONTEXT_LIST_ALL>;
    using ContextStateList = EmbeddedList<Context, int, CONTEXT_LIST_STATE>;
    using ContextChildrenList = EmbeddedList<Context, int, CONTEXT_LIST_CHILDREN>;

    struct Handle;

  protected:
    friend struct Cooperator;

    Context(
        Context* parent,
        SpawnConfiguration const& config,
        Handle* handle,
        Cooperator* cooperator);
  public:

    ~Context();

    // Return control to the cooperator so that it can schedule other contexts
    //
    bool Yield(bool force = false);

    Cooperator* GetCooperator()
    {
        return m_cooperator;
    }

    // The Killed system for contexts uses a Signal that starts armed and is notified on kill.
    //
    bool IsKilled() const
    {
        return m_killedSignal.IsSignaled();
    }

    // Why this context was killed — None while alive. The cause picks the HTTP status
    // and the metric bucket that IsKilled() alone cannot: Kill (explicit/parent),
    // Deadline (budget expired), Drain (graceful shutdown). First cause wins.
    //
    KillCause WhyKilled() const { return m_killCause; }

    // Absolute deadline (monotonic microseconds; 0 = none). Children inherit the
    // parent's deadline at spawn; setting one only ever TIGHTENS (min with existing),
    // so a callee cannot extend the budget its caller granted. Enforcement is
    // cooperative and zero-cost when unset: kill-aware waits gate on it — a wait past
    // the deadline kills the context tree with KillCause::Deadline.
    //
    int64_t DeadlineUs() const { return m_deadlineUs; }
    void SetDeadline(int64_t absoluteUs)
    {
        if (m_deadlineUs == 0 || absoluteUs < m_deadlineUs)
        {
            m_deadlineUs = absoluteUs;
        }
    }
    void SetDeadlineIn(time::Interval budget);

    // Remaining budget in microseconds; INT64_MAX when no deadline is set.
    //
    int64_t RemainingUs() const;

    // Kill hook: fn(arg) runs when this context is killed (during kill propagation,
    // before waiters wake), in kill-traversal order. The node is caller-owned and
    // stack-resident; the RAII guard deregisters on scope exit. Generalizes the
    // watcher-context pattern (io::ShutdownOnKillGuard) for cheap cases: no context,
    // no coordinator — one intrusive node. Hooks must not block.
    //
    struct KillHook : EmbeddedListHookups<KillHook>
    {
        void (*fn)(void*) = nullptr;
        void* arg = nullptr;
    };

    void OnKill(KillHook* hook) { m_killHooks.Push(hook); }
    void RemoveKillHook(KillHook* hook) { m_killHooks.Remove(hook); }

    // Kill this context's own subtree with a cause — the self-abort entry a
    // deadline-expiring wait uses. Runs on this cooperator (this == running context or
    // an ancestor of it); schedule=false since the caller is mid-wait-unwind.
    //
    void KillSelf(KillCause cause) { Kill(this, false, cause); }

    // Kill-machinery internals (public for the traversal in context.cpp; not user API)
    //
    void RecordKillCause(KillCause cause)
    {
        if (m_killCause == KillCause::None)
        {
            m_killCause = cause;
        }
    }
    void FireKillHooks()
    {
        // Fire once (a context can be reached by more than one Kill — parent tree kill
        // then a deadline). Non-destructive: hooks stay linked so their owner's
        // RemoveKillHook / scope-exit deregistration remains valid.
        //
        if (m_killHooksFired)
        {
            return;
        }
        m_killHooksFired = true;
        // Peek() on an empty list returns the (garbage) sentinel cast, not null — guard
        // with IsEmpty(); Next() correctly returns null at the sentinel.
        //
        for (auto* hook = m_killHooks.IsEmpty() ? nullptr : m_killHooks.Peek();
             hook; hook = m_killHooks.Next(hook))
        {
            hook->fn(hook->arg);
        }
    }

    // Returns the kill signal for use with Wait or CoordinateWith patterns.
    //
    Signal* GetKilledSignal()
    {
        return &m_killedSignal;
    }

    Context* Parent()
    {
        return m_parent;
    }

    const char* GetName() const
    {
        return m_name ? m_name : "[anonymous]";
    }

    void SetName(const char* name)
    {
        m_name = name;
    }

    // Bump-allocate a typed object with optional trailing bytes from this context's segment heap.
    // Returns an RAII Alloc<T> that destructs and de-bumps on scope exit. Defined in alloc.h.
    //
    template<typename T, typename... Args>
    Alloc<T> Allocate(size_t extra, Args&&... args);

    // Bump-allocate a raw byte buffer. Returns an RAII AllocBuffer. Defined in alloc.h.
    //
    AllocBuffer AllocateBuffer(size_t size);

    // Detach disassociates the context from its parent so that it will not be killed when the
    // parent is. If you have a reason to use this, it is assumed you're able to make sure no
    // there's a coherent contract where no one else is going to or already detached it.
    //
    void Detach();

  private:
    friend struct Cooperator;
    friend struct Coordinator;
    friend struct CoordinatorExtension;
    friend struct Signal;
    friend struct CompletionLatch;

    // Enter the block caused by the given coordinator
    //
    void Block();

    // Unblock the given context, switching to it if requested.
    //
    void Unblock(Context* c, const bool schedule = true);

  private:
    friend struct Handle;
    
    // Keeping with the concept that all Context APIs should only be called when it is actively
    // scheduled in the cooperator, this kills the other given context. In practice, it's also
    // simply necessary that we leverage the current context to coordinate downstream events.
    //
    void Kill(Context* other, const bool schedule = true,
              KillCause cause = KillCause::Kill);

  public:
    Context* m_parent;
    Handle* m_handle;
    SchedulerState m_state;
    int m_priority;
    int m_currentPriority;
    Cooperator* m_cooperator;
    Signal m_killedSignal;
    KillCause m_killCause{KillCause::None};
    bool m_killHooksFired{false};
    int64_t m_deadlineUs{0};
    EmbeddedList<KillHook> m_killHooks;
    ContextChildrenList m_children;
    Coordinator m_lastChild;
    const char* m_name;

    struct
    {
        size_t ticks;
        size_t yields;
        size_t blocks;
        size_t ioSubmits;
        size_t ioCompletes;
        size_t samples;
    } m_statistics;
    int64_t m_lastRdtsc;

    // Per-context epoch participation: traversal pin (Guard-managed) and application pin
    // (transaction-managed). Both default to Epoch::Unpinned() (zero). Written and read only
    // on the owning cooperator thread; the cooperator's m_epochWatermark atomic is the sole
    // cross-thread visibility boundary.
    //
    epoch::State m_epochState{};

    // Bump heap watermark — grows upward from just past the Launchable/lambda at segment bottom.
    // Set by Spawn/Launch after placing the initial object. BumpAlloc advances it; BumpFree
    // restores it (LIFO).
    //
    void* m_heapTop{nullptr};

    // Saved stack pointer — the 'bookmark' to switch back to when the context is resumed.
    //
    void* m_sp{nullptr};

    // Entry function to call when the context first starts executing. Set by Spawn/Launch before
    // calling EnterContext, which uses it from the makecontext trampoline.
    //
    void (*m_entry)(Context*);

    // Optional cleanup function called after m_entry returns but before ~Context(). Set by Launch
    // to a typed destructor trampoline so Launchable subclasses get proper C++ destruction. Null
    // for Spawn (lambdas are memcpy'd, not constructed).
    //
    void (*m_cleanup)(Context*);

    // Must be last member
    //
    Segment m_segment;
};

// Handle is the mechanism for working with contexts executing in a cooperator, both in and outsidee
// of cooperating contexts. Handle lifetimes must be guaranteed for as long as the execution of the
// spawned context.
//
// In theory, this is the obvious mechanism to add "return things" to the spawn concept. However,
// that's only really needed for outside-of-cooperator work that will probably want fancier ways
// to do things in general than what's easy to do right now, and the intra-context case, there
// isn't really any sufficiently magic syntax beyond union hijinks.
//
struct Context::Handle
{
    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;
    Handle()
    : m_context(nullptr)
    {
    }

    void Kill(KillCause cause = KillCause::Kill);

    Signal* GetKilledSignal();

    operator bool() const
    {
        return !!m_context;
    }

  private:
    friend Context;
    friend Cooperator;
    Context* m_context;
};

} // end namespace coop
