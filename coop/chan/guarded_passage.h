#pragma once

// GuardedPassage: RAII-guarded SPSC channel with safe destruction.
//
// Flexible-array layout: the base struct ends with T m_ring[0], and the
// fixed-size variant extends it with contiguous storage. Zero pointer
// indirection on the hot path — m_ring[idx] IS the data, no m_buffer
// dereference.
//
// RecvSide<T> and SendSide<T> operate on GuardedPassage<T>*, seeing only
// the base type. Ring capacity is runtime (m_capacity) but the access
// pattern is direct array indexing.
//

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <unistd.h>
#include <utility>

namespace coop::chan
{

enum class GuardedPassageState : uint8_t
{
    Created       = 0,
    RecvOnly      = 1,
    SendRecv      = 2,
    SendShutdown  = 3,
    RecvShutdown  = 4,
    Shutdown      = 5,
};

// ---------------------------------------------------------------------------
// GuardedPassage<T>: base with flexible array member.
//
// Hot path (TryPush/TryPop) is non-virtual, operates on m_ring[] directly.
//

template<typename T>
struct GuardedPassage
{
    // Use FixedGuardedPassage<T, N> to construct.
    //

    bool TryPush(T value)
    {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        size_t head = m_head.load(std::memory_order_acquire);
        if ((tail - head) == m_capacity)
            return false;
        m_ring[tail % m_capacity] = std::move(value);
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool TryPop(T& value)
    {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t tail = m_tail.load(std::memory_order_acquire);
        if (head == tail)
            return false;
        value = std::move(m_ring[head % m_capacity]);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    bool IsEmpty() const
    {
        return m_head.load(std::memory_order_relaxed) ==
               m_tail.load(std::memory_order_acquire);
    }

    GuardedPassageState State() const
    {
        return m_state.load(std::memory_order_acquire);
    }

    bool IsShutdown() const { return State() == GuardedPassageState::Shutdown; }

    // Returns whether this call performed the transition. A false return means
    // the state was not `expected` -- either a peer moved it first, or the
    // caller's pairing assumption is wrong. Side destructors below re-read and
    // retry on false; other callers are free to ignore the result.
    //
    bool TransitionTo(GuardedPassageState expected, GuardedPassageState next)
    {
        return m_state.compare_exchange_strong(expected, next,
            std::memory_order_acq_rel);
    }

    // Destructor waits for Shutdown -- the terminal state only reached once both
    // RecvSide and SendSide have run their destructors. On the healthy path that
    // handshake completes within microseconds of the second side tearing down.
    //
    // A caller can drop the last reference to a passage whose peer side never ran
    // its destructor at all (e.g. a context torn down before constructing its
    // side, or one whose side object was destroyed without transitioning state --
    // both are pairing bugs at the call site, not something this destructor can
    // detect ahead of time). Waiting unboundedly for that peer turns a call-site
    // bug into a permanently wedged thread -- worse than the bug itself, and it
    // masks the bug rather than surfacing it. So the wait is bounded: 5 seconds
    // (no existing coop precedent covers a destructor spin; this is a generous
    // multiple of the healthy-path handshake latency, chosen purely as a hang
    // backstop) with the same doubling backoff as the healthy wait. Expiry aborts
    // in every build: elapsed time cannot prove the peer has stopped accessing
    // the passage, so returning would let destruction reclaim live storage.
    //
    ~GuardedPassage() { WaitForShutdown(); }

protected:
    // The storage owner must wait before destroying its elements.
    //
    void WaitForShutdown()
    {
        if (m_state.load(std::memory_order_acquire) == GuardedPassageState::Created)
            return;

        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int backoffUs = 10;
        while (m_state.load(std::memory_order_acquire) != GuardedPassageState::Shutdown)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                GuardedPassageState stuck = m_state.load(std::memory_order_acquire);
                fprintf(stderr,
                    "coop::chan::GuardedPassage: destructor timed out waiting for "
                    "Shutdown, state stuck at %d; aborting before reclaiming live storage\n",
                    static_cast<int>(stuck));
                std::abort();
            }
            usleep(backoffUs);
            if (backoffUs < 10000)
                backoffUs *= 2;
        }
    }

    // Only constructed by FixedGuardedPassage.
    //
    explicit GuardedPassage(size_t capacity) : m_capacity(capacity) {}

    GuardedPassage(GuardedPassage const&) = delete;
    GuardedPassage& operator=(GuardedPassage const&) = delete;

    std::atomic<GuardedPassageState>  m_state{GuardedPassageState::Created};
    alignas(64) std::atomic<size_t> m_tail{0};
    alignas(64) std::atomic<size_t> m_head{0};
    size_t                     m_capacity;
    T                          m_ring[0];  // flexible array — storage follows
};

// ---------------------------------------------------------------------------
// FixedGuardedPassage<T, N>: contiguous storage for N elements.
// m_ring[0] overlaps m_storage[0].
//

template<typename T, size_t N>
struct FixedGuardedPassage : GuardedPassage<T>
{
    static_assert(N > 0, "Capacity must be positive");

    FixedGuardedPassage() : GuardedPassage<T>(N) {}
    ~FixedGuardedPassage() { this->WaitForShutdown(); }

private:
    T m_storage[N];
};

// ---------------------------------------------------------------------------
// RecvSide<T>: RAII consumer guard. Operates on GuardedPassage<T>*.
//

template<typename T>
struct RecvSide
{
    explicit RecvSide(GuardedPassage<T>& core) : m_core(&core)
    {
        core.TransitionTo(GuardedPassageState::Created, GuardedPassageState::RecvOnly);
    }

    // The peer side can be tearing down concurrently on another thread -- that is
    // the normal case for a passage bridging two cooperators. Reading the state
    // once and issuing a single CAS loses that race: both sides observe SendRecv,
    // one CAS wins, and the loser's CAS silently fails, leaving the passage parked
    // at SendShutdown/RecvShutdown with no side left alive to move it to Shutdown.
    // Re-read and retry until this side's transition actually lands.
    //
    ~RecvSide()
    {
        if (!m_core) return;
        while (true)
        {
            auto s = m_core->State();
            if (s == GuardedPassageState::RecvOnly)
            {
                if (m_core->TransitionTo(GuardedPassageState::RecvOnly,
                                         GuardedPassageState::Shutdown))
                    return;
            }
            else if (s == GuardedPassageState::SendRecv)
            {
                if (m_core->TransitionTo(GuardedPassageState::SendRecv,
                                         GuardedPassageState::RecvShutdown))
                    return;
            }
            else if (s == GuardedPassageState::SendShutdown)
            {
                if (m_core->TransitionTo(GuardedPassageState::SendShutdown,
                                         GuardedPassageState::Shutdown))
                    return;
            }
            else
            {
                // RecvShutdown/Shutdown/Created: this side is already accounted
                // for (or was never paired), and no transition is owed.
                //
                return;
            }
        }
    }

    RecvSide(RecvSide const&) = delete;
    RecvSide& operator=(RecvSide const&) = delete;
    RecvSide(RecvSide&& o) noexcept : m_core(o.m_core) { o.m_core = nullptr; }
    RecvSide& operator=(RecvSide&& o) noexcept
    {
        m_core = o.m_core;
        o.m_core = nullptr;
        return *this;
    }

    void Release()
    {
        if (!m_core) return;
        if (m_core->State() == GuardedPassageState::RecvOnly)
            m_core->TransitionTo(GuardedPassageState::RecvOnly, GuardedPassageState::Shutdown);
        m_core = nullptr;
    }

    bool TryPop(T& value) { return m_core->TryPop(value); }
    bool IsEmpty() const { return m_core->IsEmpty(); }

    bool SenderDone() const
    {
        auto s = m_core->State();
        return s == GuardedPassageState::SendShutdown || s == GuardedPassageState::Shutdown;
    }

private:
    GuardedPassage<T>* m_core;
};

// ---------------------------------------------------------------------------
// SendSide<T>: RAII producer guard. Operates on GuardedPassage<T>*.
//

template<typename T>
struct SendSide
{
    explicit SendSide(GuardedPassage<T>& core) : m_core(&core)
    {
        core.TransitionTo(GuardedPassageState::RecvOnly, GuardedPassageState::SendRecv);
    }

    // Retries for the same reason ~RecvSide does: a single read-then-CAS loses a
    // concurrent teardown race against the peer side and parks the passage one
    // transition short of Shutdown.
    //
    ~SendSide()
    {
        if (!m_core) return;
        while (true)
        {
            auto s = m_core->State();
            if (s == GuardedPassageState::SendRecv)
            {
                if (m_core->TransitionTo(GuardedPassageState::SendRecv,
                                         GuardedPassageState::SendShutdown))
                    return;
            }
            else if (s == GuardedPassageState::RecvShutdown)
            {
                if (m_core->TransitionTo(GuardedPassageState::RecvShutdown,
                                         GuardedPassageState::Shutdown))
                    return;
            }
            else
            {
                // SendShutdown/Shutdown: already accounted for. RecvOnly/Created:
                // this side's constructor never committed the handshake, so it
                // owes nothing.
                //
                return;
            }
        }
    }

    SendSide(SendSide const&) = delete;
    SendSide& operator=(SendSide const&) = delete;
    SendSide(SendSide&& o) noexcept : m_core(o.m_core) { o.m_core = nullptr; }
    SendSide& operator=(SendSide&& o) noexcept
    {
        m_core = o.m_core;
        o.m_core = nullptr;
        return *this;
    }

    bool TryPush(T value) { return m_core->TryPush(std::move(value)); }

    bool ReceiverDone() const
    {
        auto s = m_core->State();
        return s == GuardedPassageState::RecvShutdown || s == GuardedPassageState::Shutdown;
    }

private:
    GuardedPassage<T>* m_core;
};

} // namespace coop::chan
