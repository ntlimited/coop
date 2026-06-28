#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "coordinator.h"
#include "cooperator.h"
#include "detail/coordinator_extension.h"
#include "self.h"

namespace coop
{

namespace detail
{
    // Stand-in result for void-returning continuations so a single template body covers both.
    //
    struct Void {};
}

// Single-waiter, one-shot completion latch. A continuation has at most one awaiter (the context
// that called Await), so where a Coordinator carries a FIFO wait list this needs only a single
// slot — no list push/pop, no held/release bookkeeping. Single-cooperator, so no synchronization:
// Fire (from the loop drain) and Wait (from the awaiter) never overlap. Cheaper than embedding a
// full Coordinator purely to signal completion.
//
struct CompletionLatch
{
    bool Fired() const
    {
        return m_fired;
    }

    // Mark complete and wake the parked awaiter, if any. Runs from the cooperator loop's drain
    // (no current context), so the wake is schedule=false: move the awaiter to yielded.
    //
    void Fire()
    {
        m_fired = true;
        if (m_awaiter)
        {
            Cooperator::thread_cooperator->Unblock(m_awaiter, /*schedule=*/false);
            m_awaiter = nullptr;
        }
    }

    // Park the calling context until Fire(). Returns immediately if already fired.
    //
    void Wait(Context* ctx)
    {
        if (m_fired)
        {
            return;
        }
        m_awaiter = ctx;
        ctx->Block();
    }

  private:
    Context* m_awaiter = nullptr;
    bool     m_fired = false;
};

// Lambda-backed continuation. Registered on a Coordinator via Coordinator::Continue; when that
// coordinator is Released, Resume runs `fn` to completion as a function call (no context switch)
// on the releasing cooperator. The caller frame owns this object (it carries intrusive wait-list
// hooks, so it is neither movable nor copyable).
//
// Lifecycle: fired ⇒ result discarded on destruction; unfired ⇒ cancelled (unhooked) on
// destruction. Await() blocks the calling context until it fires and returns the result; Cancel()
// detaches it early. There is no kill special-casing — a kill-aware caller cancels if it cares.
//
template<typename Fn>
struct ContinuationImpl final : Continuation
{
    using Result = std::invoke_result_t<Fn&, Coordinator*>;
    static constexpr bool kVoid = std::is_void_v<Result>;
    using Stored = std::conditional_t<kVoid, detail::Void, Result>;

    ContinuationImpl(Coordinator* coord, Fn fn)
    : m_coordinated(static_cast<Continuation*>(this))
    , m_coord(coord)
    , m_fn(std::move(fn))
    {
        // Register on the target coordinator's wait list. Cooperative scheduling means no
        // completion can be processed before the current context yields, so this races nothing.
        //
        CoordinatorExtension().AddAsBlocked(coord, &m_coordinated);
    }

    ContinuationImpl(ContinuationImpl const&) = delete;
    ContinuationImpl(ContinuationImpl&&) = delete;

    ~ContinuationImpl() final
    {
        if (m_latch.Fired())                  // already fired ⇒ drop the (uncollected) result
        {
            if constexpr (!kVoid)
            {
                Stored_ptr()->~Stored();
            }
            return;
        }
        Cancel();
    }

    void Run() final
    {
        // Runs from the cooperator loop's continuation drain — there is no current context, so
        // m_coord (the coordinator this fired on) is passed to fn, and the latch wake is
        // context-free (Fire routes through the cooperator, schedule=false).
        //
        if constexpr (kVoid)
        {
            m_fn(m_coord);
        }
        else
        {
            new (&m_storage) Stored(m_fn(m_coord));
        }

        m_latch.Fire();
    }

    // Detach an unfired continuation from its coordinator so it will never fire. No-op once
    // fired. In-cooperator only (cooperative scheduling makes this race-free).
    //
    bool Cancel()
    {
        if (m_latch.Fired() || m_cancelled)
        {
            return false;
        }
        m_cancelled = true;
        CoordinatorExtension().RemoveAsBlocked(m_coord, &m_coordinated);
        return true;
    }

    // Block the calling context until the continuation fires, then return its result.
    //
    Result Await()
    {
        m_latch.Wait(Self());
        if constexpr (!kVoid)
        {
            return std::move(*Stored_ptr());
        }
    }

  private:
    Stored* Stored_ptr()
    {
        return std::launder(reinterpret_cast<Stored*>(&m_storage));
    }

    Coordinated     m_coordinated;
    CompletionLatch m_latch;
    Coordinator*    m_coord;
    Fn              m_fn;
    bool            m_cancelled = false;
    alignas(Stored) unsigned char m_storage[sizeof(Stored)];
};

template<typename Fn>
auto Coordinator::Continue(Fn&& fn)
{
    // Guaranteed copy elision: the ContinuationImpl is constructed directly in the caller's
    // storage, so its intrusive hooks are registered at their final address.
    //
    return ContinuationImpl<std::decay_t<Fn>>(this, std::forward<Fn>(fn));
}

// Detached (hoisted) continuation: owns itself rather than living on a caller frame, so there is
// no parked context awaiting it — the high-fan-out win. Registered on a coordinator at creation,
// it fires once from the loop drain, runs fn to completion, and frees itself. fn is terminal
// (no result is collected); to continue a pipeline it registers the next detached continuation.
//
// Lifetime is owner-managed, exactly like a blocked context: whoever registers it must guarantee
// it fires or is cancelled before the coordinator dies (for IO, the Handle does this). This first
// cut heap-allocates per continuation — already far cheaper than a parked stack at fan-out; a
// per-cooperator pool (single-cooperator, no atomics) is the follow-on.
//
template<typename Fn>
struct DetachedContinuationImpl final : Continuation
{
    // Allocated from the owning cooperator's per-thread continuation pool rather than the general
    // heap. DetachedContinuationImpl is final, so `delete this` in Resume sees the static type and
    // calls the sized operator delete — the pool needs the size to pick the right class.
    //
    static void* operator new(std::size_t n)
    {
        return Cooperator::thread_cooperator->AllocateContinuation(n);
    }

    static void operator delete(void* p, std::size_t n)
    {
        Cooperator::thread_cooperator->FreeContinuation(p, n);
    }

    DetachedContinuationImpl(Coordinator* coord, Fn fn)
    : m_coordinated(static_cast<Continuation*>(this))
    , m_coord(coord)
    , m_fn(std::move(fn))
    {
        CoordinatorExtension().AddAsBlocked(coord, &m_coordinated);
    }

    void Run() final
    {
        m_fn(m_coord);
        delete this;            // self-free once fired (its node was already popped by the drain)
    }

  private:
    Coordinated  m_coordinated;
    Coordinator* m_coord;
    Fn           m_fn;
};

template<typename Fn>
void Coordinator::ContinueDetached(Fn&& fn)
{
    // The continuation owns itself; the runtime frees it when it fires (Resume -> delete this).
    //
    new DetachedContinuationImpl<std::decay_t<Fn>>(this, std::forward<Fn>(fn));
}

} // end namespace coop
