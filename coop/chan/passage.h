#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

#include "coop/coordinate_with.h"
#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/self.h"
#include "coop/time/interval.h"

// Passage is a thread-safe queue bridge from external threads (or other cooperators) to a
// designated receiver cooperator.
//
// The default Passage<T, N> is MPSC. SpscPassage<T, N> provides the same API with a faster
// single-producer ring for the 1-producer case.

namespace coop
{
namespace chan
{

// ---------------------------------------------------------------------------
// MpscRing<T, N> — Dmitry Vyukov's bounded MPSC queue.
//
// Multiple producers may call Push() concurrently (wait-free CAS on m_tail).
// Exactly one consumer calls Pop() and IsEmpty() (m_head is not atomic).
// N must be a power of 2.
// ---------------------------------------------------------------------------

template<typename T, size_t N>
struct MpscRing
{
    static_assert(N > 0 && (N & (N - 1)) == 0,
        "MpscRing capacity N must be a power of 2.");
    static_assert(std::is_default_constructible_v<T>,
        "Passage value type T must be default-constructible.");
    static_assert(std::is_move_assignable_v<T>,
        "Passage value type T must be move-assignable.");

    MpscRing()
    {
        for (size_t i = 0; i < N; i++)
            m_slots[i].m_seq.store(i, std::memory_order_relaxed);
    }

    // Wait-free for producers. Returns false if the ring is full.
    //
    bool Push(T value)
    {
        size_t pos = m_tail.load(std::memory_order_relaxed);

        for (;;)
        {
            Slot& slot = m_slots[pos & (N - 1)];
            size_t seq = slot.m_seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0)
            {
                if (m_tail.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                return false;  // full
            }
            else
            {
                pos = m_tail.load(std::memory_order_relaxed);
            }
        }

        Slot& slot = m_slots[pos & (N - 1)];
        slot.m_value = std::move(value);
        slot.m_seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Consumer-only. Returns false if empty or if a producer has claimed a slot
    // but not yet committed (slot.m_seq not yet set). Caller should retry later.
    //
    bool Pop(T& value)
    {
        Slot& slot = m_slots[m_head & (N - 1)];
        size_t seq = slot.m_seq.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(m_head + 1);

        if (diff != 0)
            return false;

        value = std::move(slot.m_value);
        slot.m_seq.store(m_head + N, std::memory_order_release);
        m_head++;
        return true;
    }

    // Consumer-only emptiness check.
    //
    bool IsEmpty() const
    {
        const Slot& slot = m_slots[m_head & (N - 1)];
        size_t seq = slot.m_seq.load(std::memory_order_acquire);
        return static_cast<intptr_t>(seq) - static_cast<intptr_t>(m_head + 1) != 0;
    }

  private:
    struct Slot
    {
        std::atomic<size_t> m_seq{0};
        T                   m_value{};
    };

    alignas(64) Slot                m_slots[N];
    alignas(64) std::atomic<size_t> m_tail{0};
    alignas(64) size_t              m_head{0};
};

// ---------------------------------------------------------------------------
// SpscRing<T, N> — bounded single-producer/single-consumer ring.
//
// One producer thread calls Push(); one consumer (the target cooperator thread)
// calls Pop() / IsEmpty(). This removes producer-side CAS from the hot path.
// N must be a power of 2.
// ---------------------------------------------------------------------------

template<typename T, size_t N>
struct SpscRing
{
    static_assert(N > 0 && (N & (N - 1)) == 0,
        "SpscRing capacity N must be a power of 2.");
    static_assert(std::is_default_constructible_v<T>,
        "Passage value type T must be default-constructible.");
    static_assert(std::is_move_assignable_v<T>,
        "Passage value type T must be move-assignable.");

    bool Push(T value)
    {
#ifndef NDEBUG
        AssertSingleProducer();
#endif

        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);

        if ((tail - head) == N)
            return false;  // full

        m_slots[tail & (N - 1)] = std::move(value);
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool Pop(T& value)
    {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);

        if (head == tail)
            return false;

        value = std::move(m_slots[head & (N - 1)]);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    bool IsEmpty() const
    {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        return head == tail;
    }

  private:
#ifndef NDEBUG
    void AssertSingleProducer()
    {
        std::lock_guard<std::mutex> lock(m_ownerMutex);

        const std::thread::id me = std::this_thread::get_id();
        if (!m_ownerSet)
        {
            m_owner = me;
            m_ownerSet = true;
            return;
        }

        assert(m_owner == me && "SpscRing::Push called from multiple producer threads");
    }

    std::thread::id m_owner;
    bool            m_ownerSet{false};
    std::mutex      m_ownerMutex;
#endif

    alignas(64) T                      m_slots[N]{};
    alignas(64) std::atomic<size_t>    m_tail{0};  // producer writes
    alignas(64) std::atomic<size_t>    m_head{0};  // consumer writes
};

// ---------------------------------------------------------------------------

template<typename T, size_t N, template<typename, size_t> class Ring>
struct BasicPassage
{
    BasicPassage(const BasicPassage&) = delete;
    BasicPassage(BasicPassage&&)      = delete;

    struct RecvTuning
    {
        int     yieldThreshold{8};
        int64_t timeoutInitialUs{10};
        int64_t timeoutMaxUs{10000};
        int     timeoutBackoff{4};
    };

    // ctx:    the receiver cooperator's current context.
    // target: the cooperator that runs the consumer (typically ctx->GetCooperator()).
    //
    explicit BasicPassage(Context* ctx, Cooperator* target, RecvTuning tuning = {})
    : m_state(std::make_shared<State>(ctx))
    , m_target(target)
    , m_tuning(tuning)
    , m_recvTimeoutUs(tuning.timeoutInitialUs)
    {
        assert(ctx && "Passage: ctx must be non-null");
        assert(target && "Passage: target must be non-null");
        assert(ctx->GetCooperator() == target
               && "Passage: ctx must belong to target cooperator");
        assert(m_tuning.yieldThreshold >= 0);
        assert(m_tuning.timeoutInitialUs > 0);
        assert(m_tuning.timeoutMaxUs >= m_tuning.timeoutInitialUs);
        assert(m_tuning.timeoutBackoff >= 2);
    }

    ~BasicPassage() { Shutdown(); }

    // Thread-safe. Callable from any thread or cooperator.
    // Returns false if the passage is shut down or the ring is full.
    //
    bool Send(T value);

    // Receiver-only, non-blocking pop. Returns false if no item is currently available.
    //
    bool TryRecv(T& value);

    // Receiver-only, non-blocking batch pop.
    //
    size_t Drain(T* out, size_t maxCount);

    // Receive one item from the passage on the receiver cooperator. Returns false when
    // the passage is shut down and the ring is empty.
    //
    bool Recv(T& value);

    // Thread-safe, idempotent shutdown.
    //
    void Shutdown();

  private:
    struct State
    {
        explicit State(Context* ctx)
        : m_recv(ctx)
        {
        }

        Ring<T, N>        m_ring;
        Coordinator       m_recv;          // held <-> ring empty
        std::atomic<bool> m_shutdown{false};
        std::atomic<bool> m_wakePending{false};
    };

    std::shared_ptr<State>  m_state;
    Cooperator*             m_target;
    RecvTuning              m_tuning;

    // Receiver-local adaptive Recv state.
    //
    int     m_recvDryCount{0};
    int64_t m_recvTimeoutUs{0};

    bool SubmitWake(bool releaseOnEmpty);
};

// ---------------------------------------------------------------------------
// BasicPassage::SubmitWake
// ---------------------------------------------------------------------------

template<typename T, size_t N, template<typename, size_t> class Ring>
bool BasicPassage<T, N, Ring>::SubmitWake(bool releaseOnEmpty)
{
    auto state = m_state;

    bool expected = false;
    if (!state->m_wakePending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
    {
        return true;  // wake already queued/in-flight
    }

    auto wakeFn = [state, releaseOnEmpty](Context* ctx)
    {
        state->m_wakePending.store(false, std::memory_order_seq_cst);

        // Release on explicit wake requests, non-empty ring, or shutdown.
        //
        if ((releaseOnEmpty
             || state->m_shutdown.load(std::memory_order_acquire)
             || !state->m_ring.IsEmpty())
            && state->m_recv.IsHeld())
        {
            state->m_recv.Release(ctx);
        }
    };

    bool submitted = Cooperator::thread_cooperator
        ? m_target->Cooperate(std::move(wakeFn))
        : m_target->Submit(std::move(wakeFn));

    if (!submitted)
    {
        state->m_wakePending.store(false, std::memory_order_seq_cst);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// BasicPassage::Send
// ---------------------------------------------------------------------------

template<typename T, size_t N, template<typename, size_t> class Ring>
bool BasicPassage<T, N, Ring>::Send(T value)
{
    auto state = m_state;

    if (state->m_shutdown.load(std::memory_order_acquire))
        return false;

    // If the receiver cooperator is shutting down, stop accepting sends.
    //
    if (m_target->IsShuttingDown())
    {
        state->m_shutdown.store(true, std::memory_order_release);
        return false;
    }

    if (!state->m_ring.Push(std::move(value)))
        return false;  // ring full

    // Submit failure can happen if target enters shutdown between push and submit.
    //
    if (!SubmitWake(false))
    {
        state->m_shutdown.store(true, std::memory_order_release);
        return false;
    }

    // If shutdown starts now, prevent additional sends. This item may still be delivered.
    //
    if (m_target->IsShuttingDown())
    {
        state->m_shutdown.store(true, std::memory_order_release);
    }

    return true;
}

// ---------------------------------------------------------------------------
// BasicPassage::TryRecv
// ---------------------------------------------------------------------------

template<typename T, size_t N, template<typename, size_t> class Ring>
bool BasicPassage<T, N, Ring>::TryRecv(T& value)
{
    assert(Cooperator::thread_cooperator == m_target
           && "Passage::TryRecv must run on the target cooperator");
    Context* ctx = Self();

    if (!m_state->m_ring.Pop(value))
        return false;

    m_recvDryCount  = 0;
    m_recvTimeoutUs = m_tuning.timeoutInitialUs;

    if (m_state->m_ring.IsEmpty() && !m_state->m_recv.IsHeld())
        m_state->m_recv.Acquire(ctx);

    return true;
}

// ---------------------------------------------------------------------------
// BasicPassage::Drain
// ---------------------------------------------------------------------------

template<typename T, size_t N, template<typename, size_t> class Ring>
size_t BasicPassage<T, N, Ring>::Drain(T* out, size_t maxCount)
{
    assert(Cooperator::thread_cooperator == m_target
           && "Passage::Drain must run on the target cooperator");
    if (!out || maxCount == 0)
        return 0;

    Context* ctx = Self();
    size_t n = 0;

    while (n < maxCount && m_state->m_ring.Pop(out[n]))
        n++;

    if (n == 0)
        return 0;

    m_recvDryCount  = 0;
    m_recvTimeoutUs = m_tuning.timeoutInitialUs;

    if (m_state->m_ring.IsEmpty() && !m_state->m_recv.IsHeld())
        m_state->m_recv.Acquire(ctx);

    return n;
}

// ---------------------------------------------------------------------------
// BasicPassage::Recv
// ---------------------------------------------------------------------------

template<typename T, size_t N, template<typename, size_t> class Ring>
bool BasicPassage<T, N, Ring>::Recv(T& value)
{
    assert(Cooperator::thread_cooperator == m_target
           && "Passage::Recv must run on the target cooperator");

    Context* ctx = Self();

    while (true)
    {
        if (TryRecv(value))
            return true;

        if (m_state->m_shutdown.load(std::memory_order_acquire))
        {
            if (m_state->m_recv.IsHeld())
                m_state->m_recv.Release(ctx);
            return false;
        }

        // Ring is empty. Ensure m_recv is held before entering the blocking phases.
        //
        if (!m_state->m_recv.IsHeld())
            m_state->m_recv.Acquire(ctx);

        // Give Poll() a chance to process the wake submission before timed wait.
        //
        if (m_recvDryCount < m_tuning.yieldThreshold)
        {
            m_recvDryCount++;
            Yield();
            continue;
        }

        m_recvDryCount = 0;

        auto r = CoordinateWithKill(ctx, &m_state->m_recv,
                                    time::Interval(m_recvTimeoutUs));

        if (r.Killed())
            return false;

        if (r == &m_state->m_recv)
        {
            m_recvTimeoutUs = m_tuning.timeoutInitialUs;

            if (!m_state->m_ring.Pop(value))
            {
                // Shutdown released m_recv with an empty ring.
                //
                assert(m_state->m_shutdown.load());
                m_state->m_recv.Release(ctx);
                return false;
            }

            if (!m_state->m_ring.IsEmpty())
                m_state->m_recv.Release(ctx);

            return true;
        }

        // Timeout: grow adaptively and retry.
        //
        m_recvTimeoutUs = std::min(
            m_recvTimeoutUs * static_cast<int64_t>(m_tuning.timeoutBackoff),
            m_tuning.timeoutMaxUs);
    }
}

// ---------------------------------------------------------------------------
// BasicPassage::Shutdown
// ---------------------------------------------------------------------------

template<typename T, size_t N, template<typename, size_t> class Ring>
void BasicPassage<T, N, Ring>::Shutdown()
{
    auto state = m_state;
    if (state->m_shutdown.exchange(true, std::memory_order_acq_rel))
        return;

    // On the target cooperator, release directly. Otherwise submit a wake lambda.
    //
    if (Cooperator::thread_cooperator == m_target)
    {
        Context* ctx = Self();
        if (state->m_recv.IsHeld())
            state->m_recv.Release(ctx);
        return;
    }

    (void)SubmitWake(true);
}

template<typename T, size_t N = 64>
using MpscPassage = BasicPassage<T, N, MpscRing>;

template<typename T, size_t N = 64>
using Passage = MpscPassage<T, N>;

template<typename T, size_t N = 64>
using SpscPassage = BasicPassage<T, N, SpscRing>;

} // namespace chan
} // namespace coop
