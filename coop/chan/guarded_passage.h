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
#include <cstdint>
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

    void TransitionTo(GuardedPassageState expected, GuardedPassageState next)
    {
        m_state.compare_exchange_strong(expected, next,
            std::memory_order_acq_rel);
    }

    // Destructor waits for Shutdown.
    //
    ~GuardedPassage()
    {
        if (m_state.load(std::memory_order_acquire) == GuardedPassageState::Created)
            return;
        int backoffUs = 10;
        while (m_state.load(std::memory_order_acquire) != GuardedPassageState::Shutdown)
        {
            usleep(backoffUs);
            if (backoffUs < 10000)
                backoffUs *= 2;
        }
    }

protected:
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

    ~RecvSide()
    {
        if (!m_core) return;
        auto s = m_core->State();
        if (s == GuardedPassageState::RecvOnly)
            m_core->TransitionTo(GuardedPassageState::RecvOnly, GuardedPassageState::Shutdown);
        else if (s == GuardedPassageState::SendRecv)
            m_core->TransitionTo(GuardedPassageState::SendRecv, GuardedPassageState::RecvShutdown);
        else if (s == GuardedPassageState::SendShutdown)
            m_core->TransitionTo(GuardedPassageState::SendShutdown, GuardedPassageState::Shutdown);
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

    ~SendSide()
    {
        if (!m_core) return;
        auto s = m_core->State();
        if (s == GuardedPassageState::SendRecv)
            m_core->TransitionTo(GuardedPassageState::SendRecv, GuardedPassageState::SendShutdown);
        else if (s == GuardedPassageState::RecvShutdown)
            m_core->TransitionTo(GuardedPassageState::RecvShutdown, GuardedPassageState::Shutdown);
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
