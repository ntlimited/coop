#include <cassert>

#include "coordinator.h"

#include "context.h"
#include "cooperator.h"

namespace coop
{

// Coordinated packs its continuation tag and satisfied flag into the low bits of the waiter pointer,
// which is sound only if the pointed-to types are at least 4-byte aligned. Both far exceed that
// (Context is cache-line aligned; Continuation holds pointers), but assert it where the types are
// complete so a future layout change that broke the invariant fails loudly here.
//
static_assert(alignof(Context) >= 4 && alignof(Continuation) >= 4,
    "Coordinated packs two flag bits into the low bits of the waiter pointer");

namespace detail
{
bool CooperatorIsShuttingDown()
{
    auto* co = Cooperator::thread_cooperator;
    return co != nullptr && co->IsShuttingDown();
}
}

Coordinator::Coordinator()
: m_held(false)
{
}

bool Coordinator::IsHeld() const
{
    return m_held;
}

bool Coordinator::TryAcquire(Context*)
{
    if (!m_held)
    {
        m_held = true;
        return true;
    }

    return false;
}

void Coordinator::Acquire(Context* ctx)
{
    if (!m_held)
    {
        m_held = true;
        return;
    }

    Coordinated coord(ctx);
    AddAsBlocked(&coord);

    // Block the context on this coordinator
    //
    ctx->Block();
}

void Coordinator::Flash(Context* ctx)
{
    if (!m_held)
    {
        return;
    }
    Acquire(ctx);
    Release(ctx);
}

void Coordinator::Release(Context* ctx, const bool schedule /* = true */)
{
    // Allow no-op release when the coordinator is not held. This supports Signal::Notify which
    // clears m_held before MultiCoordinator cleanup can call Release.
    //
    if (!m_held)
    {
        return;
    }
    m_held = false;

    // Pass control to the next in line blocked on the coordinator, if it exists.
    //
    auto* next = m_blocking.Pop();
    if (!next)
    {
        return;
    }

    ReleaseToNext(next, schedule);
}

[[gnu::noinline]] void Coordinator::ReleaseToNext(Coordinated* next, const bool schedule)
{
    // A context waiter takes ownership — the coordinator stays held until it releases. A
    // continuation does not own anything; the coordinator stays released and the continuation
    // is queued to run from the cooperator loop's drain.
    //
    if (!next->IsContinuation())
    {
        m_held = true;
    }

    // Waking is routed through the cooperator (not the releasing context), so it works whether
    // Release is driven by a context or by the cooperator loop (e.g. a CQE / a continuation's
    // latch release). This is the single dispatch every wake site shares.
    //
    Cooperator::thread_cooperator->WakeWaiter(next, schedule);
}

void Coordinator::AddAsBlocked(Coordinated* c)
{
    c->Reset();
    m_blocking.Push(c);
}

void Coordinator::RemoveAsBlocked(Coordinated* c)
{
    m_blocking.Remove(c);
}

} // end namespace coop
