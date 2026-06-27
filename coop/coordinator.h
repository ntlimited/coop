#pragma once

#include <cassert>
#include <cstdint>

#include "detail/embedded_list.h"

namespace coop
{

struct Coordinator;
struct Context;
struct CoordinatorExtension;
struct Signal;

namespace detail
{
    // Defined in coordinator.cpp. True when the current thread's cooperator is tearing down. Used
    // only by the debug leak assert in ~Coordinator, to allow the one accepted case where a
    // coordinator dies with a non-kill-aware waiter still queued: cooperator teardown deliberately
    // abandons such waiters (the process is ending). Forward-declared as a free function so this
    // header need not pull in cooperator.h (which includes it).
    //
    bool CooperatorIsShuttingDown();
}

// Continuation is a stackless, run-to-completion unit that can wait on a Coordinator in place
// of a blocked Context. When the coordinator is Released, Resume runs as a function call (no
// context switch) on the releasing cooperator. Continuations are single-cooperator: registered,
// fired, and cancelled on one cooperator. See continuation.h for the lambda helper.
//
struct Continuation
{
    virtual void Resume() = 0;
    virtual ~Continuation() = default;
};

// Coordinated is the link between a waiter and the Coordinator it is waiting on. Each instance
// sits on a Coordinator's wait list and tracks whether the coordination was satisfied. The
// waiter is either a blocked Context (woken on Release) or a Continuation (Resumed on Release);
// the two are distinguished by a tag bit in the stored pointer (both are aligned).
//
struct Coordinated : EmbeddedListHookups<Coordinated>
{
    using List = EmbeddedList<Coordinated>;

    Coordinated(Context* ctx)
    : m_waiter(reinterpret_cast<uintptr_t>(ctx))
    , m_satisfied(false)
    {
    }

    Coordinated(Continuation* cont)
    : m_waiter(reinterpret_cast<uintptr_t>(cont) | kContinuationTag)
    , m_satisfied(false)
    {
    }

    Coordinated()
    : m_waiter(0)
    , m_satisfied(false)
    {
    }

    // Destroying a Coordinated while it is still on a Coordinator's wait list would corrupt
    // the list.
    //
    ~Coordinated()
    {
        assert(Disconnected());
    }

    bool Satisfied() const
    {
        return m_satisfied;
    }

    bool IsContinuation() const
    {
        return (m_waiter & kContinuationTag) != 0;
    }

  private:
    friend struct Coordinator;
    friend struct CoordinatorExtension;
    friend struct Signal;
    friend struct Cooperator;

    static constexpr uintptr_t kContinuationTag = 1;

    void Satisfy()
    {
        m_satisfied = true;
    }

    void Reset()
    {
        m_satisfied = false;
    }

    void SetContext(Context* ctx)
    {
        m_waiter = reinterpret_cast<uintptr_t>(ctx);
    }

    Context* GetContext()
    {
        return reinterpret_cast<Context*>(m_waiter & ~kContinuationTag);
    }

    Continuation* GetContinuation()
    {
        return reinterpret_cast<Continuation*>(m_waiter & ~kContinuationTag);
    }

    uintptr_t   m_waiter;
    bool        m_satisfied;
};

// Coordinator is the sole coordination primitive for contexts. It is heavily used and non-virtual,
// which also means that higher level constructs are not themselves Coordinators.
//
// It is generally better form to take a pointer to a Coordinator as an argument to build higher
// level items, versus embed one's own Coordinator.
//
struct Coordinator
{
    Coordinator(const Coordinator&) = delete;
    Coordinator(Coordinator&&) = delete;

    // Start out unheld
    //
    Coordinator();

    // Start out held. The context argument is vestigial — a coordinator tracks only whether it
    // is held, not by whom; ownership (for a mutex, deadlock detection, etc.) belongs to a type
    // layered on top, not the base coordination primitive.
    //
    Coordinator(Context*)
    : m_held(true)
    {
    }

    // A coordinator destroyed with waiters still queued leaks them: a blocked context, or a
    // registered continuation, that will now never be serviced. Catch it at the bug site rather
    // than later, when the orphaned node dangles. The one accepted exception is cooperator
    // teardown, where a non-kill-aware waiter is deliberately abandoned (the process is ending) —
    // the shutdown guard permits that. Debug-only: the whole body compiles out under NDEBUG,
    // leaving a trivial destructor on the hot path.
    //
    ~Coordinator()
    {
        assert((m_blocking.IsEmpty() || detail::CooperatorIsShuttingDown())
               && "coordinator destroyed with waiters still queued");
    }

    bool IsHeld() const;

    // TryAcquire, Acquire, and Release all act in the manner that one would expect from a mutex
    // analogue. For multi-coordinator and timeout behaviors, see the CoordinateWith functionality
    // in coordinate_with.h.
    //
    // TryAcquire takes no owner — it only marks the coordinator held — so it is callable from a
    // continuation (which has no context). Acquire still takes the calling context because it may
    // need to block it.
    //
    bool TryAcquire(Context* = nullptr);

    void Acquire(Context*);

    // Barrier: blocks until the current holder releases, then immediately passes through. No-op
    // if the coordinator is not held.
    //
    void Flash(Context*);

    // Release the coordinator, unblocking the context (if one exists) at the head of the wait
    // list. By default (schedule = true), the unblocked context will immediately be switched to
    // and execute; alternatively, Release can be made to return immediately and simply switch
    // the unblocked context to yield.
    //
    void Release(Context*, const bool schedule = true);

    // Register a stackless continuation to run when this coordinator is next Released, instead
    // of blocking a context on it. Returns a ContinuationImpl owned by the caller's frame
    // (constructed in place — guaranteed copy elision); call Await() to block until it fires and
    // collect its result, or Cancel() to detach it. Defined in continuation.h.
    //
    template<typename Fn>
    auto Continue(Fn&& fn);

    // Register a self-owning (detached) continuation: no awaiter, no parked context. It fires
    // once from the loop drain and frees itself; fn is terminal. Lifetime is owner-managed (fire
    // or cancel before the coordinator dies). Defined in continuation.h.
    //
    template<typename Fn>
    void ContinueDetached(Fn&& fn);

    // The lack of move and copy semantics make this true
    //
    bool operator==(Coordinator* other) const
    {
        return this == other;
    }

  private:
    friend struct CoordinatorExtension;
    friend struct Signal;

    void AddAsBlocked(Coordinated*);
    void RemoveAsBlocked(Coordinated*);

    // Out-of-line wake of a popped waiter. Kept off Release's body so the uncontended
    // (empty wait list) fast path sets up no frame for the wake it never reaches.
    //
    void ReleaseToNext(Coordinated* next, const bool schedule);

  private:
    bool m_held;
    Coordinated::List m_blocking;
};

} // end namespace coop
