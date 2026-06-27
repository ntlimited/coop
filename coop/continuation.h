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
    , m_completed(Self())            // held by the registrant until the continuation fires
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
        if (m_completed.TryAcquire(Self()))   // latch released ⇒ already fired ⇒ drop the result
        {
            if constexpr (!kVoid)
            {
                if (m_fired) Stored_ptr()->~Stored();
            }
            return;
        }
        Cancel();
    }

    void Resume() final
    {
        // Runs from the cooperator loop's continuation drain — there is no current context, so
        // m_coord (the coordinator this fired on) is passed to fn, and the latch wake is
        // context-free (routed through the cooperator, schedule=false).
        //
        if constexpr (kVoid)
        {
            m_fn(m_coord);
        }
        else
        {
            new (&m_storage) Stored(m_fn(m_coord));
        }
        m_fired = true;

        m_completed.Release(nullptr, /*schedule=*/false);
    }

    // Detach an unfired continuation from its coordinator so it will never fire. No-op once
    // fired. In-cooperator only (cooperative scheduling makes this race-free).
    //
    bool Cancel()
    {
        if (m_fired || m_cancelled)
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
        m_completed.Flash(Self());
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
    Coordinator     m_completed;
    Coordinator*    m_coord;
    Fn              m_fn;
    bool            m_fired = false;
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

    void Resume() final
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
