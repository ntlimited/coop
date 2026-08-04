#pragma once

#include <cstddef>

#include "coop/detail/embedded_list.h"
#include "coop/time/interval.h"

namespace coop
{

struct Context;
struct Coordinator;
struct Semaphore;

// Permit: RAII ownership of acquired semaphore units. Movable — including into a
// spawned child context, which is how a bounded-fan-out scope hands admission to its
// children. Empty permits (default-constructed, moved-from, or failed acquires) are
// falsy and release nothing.
//
struct Permit
{
    Permit() = default;
    Permit(Permit const&) = delete;
    Permit& operator=(Permit const&) = delete;

    Permit(Permit&& other) noexcept
    : m_semaphore(other.m_semaphore)
    , m_units(other.m_units)
    {
        other.m_semaphore = nullptr;
        other.m_units = 0;
    }

    Permit& operator=(Permit&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_semaphore = other.m_semaphore;
            m_units = other.m_units;
            other.m_semaphore = nullptr;
            other.m_units = 0;
        }
        return *this;
    }

    ~Permit() { Release(); }

    explicit operator bool() const { return m_semaphore != nullptr; }
    size_t Units() const { return m_units; }

    // Return the units early (idempotent; the destructor is the usual path).
    //
    void Release();

    // Split off `n` units into a new Permit (n <= Units()) — the semaphore-units
    // shape for handing part of an admission to a child.
    //
    Permit Split(size_t n);

private:
    friend struct Semaphore;
    Permit(Semaphore* semaphore, size_t units) : m_semaphore(semaphore), m_units(units) {}

    Semaphore* m_semaphore = nullptr;
    size_t     m_units = 0;
};

// Semaphore: a counting admission gate, and the reference pattern for building
// higher-level coordination on Coordinator. Each blocked acquirer parks on its own
// stack-resident Coordinator (exactly as io::Handle parks on its completion
// coordinator); a Release grants waiters strictly FIFO and wakes them by releasing
// their coordinators. Single-cooperator, like Coordinator itself: all contexts
// touching one Semaphore live on the same cooperator, and no atomics are involved.
//
// Zero cost when unused: a Semaphore is two words plus an empty list; nothing exists
// until one is constructed, and the uncontended acquire is a counter check.
//
// FIFO admission is strict even for small requests: a large waiter at the head is not
// starved by small acquires slipping past it (no barging).
//
struct Semaphore
{
    explicit Semaphore(size_t units) : m_units(units) {}

    Semaphore(Semaphore const&) = delete;
    Semaphore& operator=(Semaphore const&) = delete;

    // Non-blocking: empty permit if the units are not immediately available (or
    // waiters are queued ahead — no barging).
    //
    Permit TryAcquire(size_t n = 1);

    // Blocking, in FIFO order. Not kill-aware — safe for cleanup paths, mirroring
    // Coordinator::Acquire.
    //
    Permit Acquire(Context* ctx, size_t n = 1);

    // Kill-aware / deadline-bounded variants: empty permit when kill or timeout wins.
    // The reservation is withdrawn on abandonment; a grant that raced the wakeup is
    // returned to the pool, so no units leak.
    //
    Permit AcquireKill(Context* ctx, size_t n = 1);
    Permit AcquireKill(Context* ctx, size_t n, time::Interval timeout);

    // Add units (the manual-release escape hatch; Permit is the usual path). Grants
    // eligible waiters FIFO.
    //
    void Release(size_t n = 1);

    size_t Available() const { return m_units; }
    size_t Waiters() const { return m_waiterCount; }

private:
    // A parked acquirer, resident on its own stack. granted is written by the
    // releaser before its coordinator is released, so an abandoning waiter (kill or
    // timeout) can distinguish "woken with units" from "woken to give up".
    //
    struct Waiter : EmbeddedListHookups<Waiter>
    {
        explicit Waiter(size_t n) : need(n) {}

        Coordinator* coord = nullptr;   // points at the stack coordinator in Acquire
        size_t       need;
        bool         granted = false;
    };

    void GrantWaiters();
    Permit AcquireSlow(Context* ctx, size_t n, bool killAware,
                       time::Interval const* timeout);

    size_t               m_units;
    size_t               m_waiterCount = 0;
    EmbeddedList<Waiter> m_waitersList;
};

} // end namespace coop
