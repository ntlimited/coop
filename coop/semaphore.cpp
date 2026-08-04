#include "semaphore.h"

#include <cassert>

#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"

namespace coop
{

void Permit::Release()
{
    if (m_semaphore)
    {
        m_semaphore->Release(m_units);
        m_semaphore = nullptr;
        m_units = 0;
    }
}

Permit Permit::Split(size_t n)
{
    assert(n <= m_units && "cannot split off more units than the permit holds");
    m_units -= n;
    return Permit(m_semaphore, n);
}

Permit Semaphore::TryAcquire(size_t n /* = 1 */)
{
    // No barging: units behind a queued waiter are spoken for.
    //
    if (!m_waitersList.IsEmpty() || m_units < n)
    {
        return {};
    }
    m_units -= n;
    return Permit(this, n);
}

Permit Semaphore::Acquire(Context* ctx, size_t n /* = 1 */)
{
    if (auto permit = TryAcquire(n))
    {
        return permit;
    }
    return AcquireSlow(ctx, n, false, nullptr);
}

Permit Semaphore::AcquireKill(Context* ctx, size_t n /* = 1 */)
{
    if (auto permit = TryAcquire(n))
    {
        return permit;
    }
    return AcquireSlow(ctx, n, true, nullptr);
}

Permit Semaphore::AcquireKill(Context* ctx, size_t n, time::Interval timeout)
{
    if (auto permit = TryAcquire(n))
    {
        return permit;
    }
    return AcquireSlow(ctx, n, true, &timeout);
}

Permit Semaphore::AcquireSlow(Context* ctx, size_t n, bool killAware,
                              time::Interval const* timeout)
{
    // Park exactly as io::Handle parks on its completion coordinator: a stack-resident
    // Coordinator held by this context; the granter releases it to wake us. Everything
    // here is single-cooperator, so grant and abandonment cannot race — whichever runs
    // first sees a consistent list.
    //
    Coordinator coord;
    Waiter waiter(n);
    waiter.coord = &coord;

    coord.TryAcquire(ctx);
    m_waitersList.Push(&waiter);
    m_waiterCount++;

    bool abandoned = false;
    if (!killAware)
    {
        CoordinateWith(ctx, &coord);
    }
    else if (timeout)
    {
        auto result = CoordinateWithKill(ctx, &coord, *timeout);
        abandoned = result.Killed() || result.TimedOut();
    }
    else
    {
        auto result = CoordinateWithKill(ctx, &coord);
        abandoned = result.Killed();
    }

    if (waiter.granted)
    {
        m_waiterCount--;
        if (abandoned)
        {
            // The grant raced our wakeup reason; hand the units straight back so
            // nothing leaks and the next waiter is considered.
            //
            Release(n);
            return {};
        }
        return Permit(this, n);
    }

    // Not granted: withdraw the reservation. The coordinator is still held from our
    // own TryAcquire; release it so the stack coordinator unwinds cleanly.
    //
    m_waitersList.Remove(&waiter);
    m_waiterCount--;
    if (coord.IsHeld())
    {
        coord.Release(ctx, false);
    }
    return {};
}

void Semaphore::Release(size_t n /* = 1 */)
{
    m_units += n;
    GrantWaiters();
}

void Semaphore::GrantWaiters()
{
    // Strict FIFO: a large waiter at the head blocks smaller ones behind it (no
    // starvation by barging). granted is written before the wake so an abandoning
    // waiter can tell "woken with units" from "woken to give up".
    //
    while (!m_waitersList.IsEmpty())
    {
        auto* waiter = m_waitersList.Peek();
        if (waiter->need > m_units)
        {
            break;
        }
        m_units -= waiter->need;
        m_waitersList.Pop();
        waiter->granted = true;
        waiter->coord->Release(nullptr, false);
    }
}

} // end namespace coop
