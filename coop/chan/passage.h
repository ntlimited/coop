#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include "coop/coordinate_with.h"
#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/self.h"
#include "coop/time/interval.h"

// Passage<T, N> is a thread-safe single-producer-or-multiple-producer, single-consumer
// channel that bridges external threads (or other cooperators) to a designated receiver
// cooperator.
//
// Architecture:
//   External threads push items into MpscRing<T,N> (wait-free CAS on producers, no mutex).
//   Exactly one wake submission is outstanding at a time (m_wakePending CAS). The wake
//   lambda — an O(1) coordinator release — runs on the target cooperator and unblocks the
//   consumer directly. The consumer pops from the ring itself; there is no intermediate
//   forwarding channel or drain context.
//
//   The m_recv Coordinator encodes ring state: held ↔ ring empty, not held ↔ ring has
//   items. When a wake lambda releases m_recv, the consumer wakes up and pops directly.
//
//   All shared state lives in a shared_ptr<State> so wake lambdas captured in the Submit
//   queue cannot access freed memory if the Passage handle is destroyed in flight.
//
// Usage:
//   // Create on the receiver cooperator's context:
//   coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());
//
//   // From any thread (non-blocking; returns false when ring is full or shut down):
//   passage.Send(42);
//
//   // On the receiver cooperator:
//   int v;
//   while (passage.Recv(v)) { ... }
//
// Recv() uses an adaptive three-phase strategy:
//   1. TryRecv fast path (pop from ring directly, no blocking).
//   2. Up to kYieldThreshold cooperative yields (gives Poll() chances to process
//      the wake Submit's CQE and deliver the coordinator release).
//   3. CoordinateWithKill with an adaptive timeout SQE (10μs → 10ms) as a backstop.
//
// Back-pressure: Send() returns false when the ring holds N items.
//
// Shutdown: ~Passage() calls Shutdown(). After shutdown, Send() returns false. Recv()
//   delivers any items remaining in the ring, then returns false. Shutdown() is idempotent.
//   A Send racing with Shutdown may still enqueue and be delivered.
//
// Thread-safety: Send() is safe to call from any thread concurrently.
//   Recv()/TryRecv()/Drain() must be called from the receiver cooperator's context.
//   Shutdown() is thread-safe.
//
// Requirement: T must be default-constructible and move-assignable.
// Note: Passage is non-copyable and non-movable.
//

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
            Slot& slot  = m_slots[pos & (N - 1)];
            size_t seq  = slot.m_seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0)
            {
                if (m_tail.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed))
                    break;
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
        Slot& slot    = m_slots[m_head & (N - 1)];
        size_t seq    = slot.m_seq.load(std::memory_order_acquire);
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

template<typename T, size_t N = 64>
struct Passage
{
    Passage(const Passage&) = delete;
    Passage(Passage&&)      = delete;

    struct RecvTuning
    {
        int     yieldThreshold{8};
        int64_t timeoutInitialUs{10};
        int64_t timeoutMaxUs{10000};
        int     timeoutBackoff{4};
    };

    // ctx:    the receiver cooperator's current context (for internal channel init).
    // target: the cooperator that runs the consumer — drain submissions are posted here.
    //         Typically: ctx->GetCooperator().
    //
    explicit Passage(Context* ctx, Cooperator* target, RecvTuning tuning = {})
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

    ~Passage() { Shutdown(); }

    // Thread-safe. Callable from any thread or cooperator.
    // Returns false if the Passage is shut down or the external ring is full.
    //
    bool Send(T value);

    // Receiver-only, non-blocking pop. Returns false if no item is currently
    // available (empty ring or producer has claimed but not committed a slot).
    //
    bool TryRecv(T& value);

    // Receiver-only, non-blocking batch pop. Drains up to maxCount items into
    // out[] and returns the number drained.
    //
    size_t Drain(T* out, size_t maxCount);

    // Receive one item from the Passage on the receiver cooperator. Returns false when
    // the Passage is shut down and the ring is empty. Uses an adaptive three-phase
    // strategy: TryRecv fast path → yield loop → CoordinateWithKill with timeout.
    //
    // Must be called from the receiver cooperator's context only.
    //
    bool Recv(T& value);

    // Shuts down the Passage. Idempotent. Items already in the ring are still delivered
    // by Recv(). Send() calls after shutdown return false; sends racing with shutdown
    // may still be delivered. Thread-safe.
    //
    void Shutdown();

  private:
    struct State
    {
        explicit State(Context* ctx) : m_recv(ctx) {}

        MpscRing<T, N>    m_ring;
        Coordinator       m_recv;          // held ↔ ring empty
        std::atomic<bool> m_shutdown{false};
        std::atomic<bool> m_wakePending{false};
    };

    std::shared_ptr<State>  m_state;
    Cooperator*             m_target;
    RecvTuning              m_tuning;

    // Adaptive recv state — receiver cooperator only, not shared.
    //
    int     m_recvDryCount{0};
    int64_t m_recvTimeoutUs{0};

    bool SubmitWake(bool releaseOnEmpty);
};

// ---------------------------------------------------------------------------
// Passage::SubmitWake
// ---------------------------------------------------------------------------

template<typename T, size_t N>
bool Passage<T, N>::SubmitWake(bool releaseOnEmpty)
{
    auto state = m_state;

    bool expected = false;
    if (!state->m_wakePending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
    {
        return true;  // wake already queued/in-flight
    }

    bool submitted = m_target->Submit([state, releaseOnEmpty](Context* ctx)
    {
        state->m_wakePending.store(false, std::memory_order_seq_cst);

        // Release on explicit wake requests, non-empty ring, or shutdown.
        // The shutdown branch closes the case where a shutdown races with an
        // already in-flight wake submission.
        //
        if ((releaseOnEmpty
             || state->m_shutdown.load(std::memory_order_acquire)
             || !state->m_ring.IsEmpty())
            && state->m_recv.IsHeld())
        {
            state->m_recv.Release(ctx);
        }
    });

    if (!submitted)
    {
        state->m_wakePending.store(false, std::memory_order_seq_cst);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Passage::Send
// ---------------------------------------------------------------------------

template<typename T, size_t N>
bool Passage<T, N>::Send(T value)
{
    auto state = m_state;

    if (state->m_shutdown.load(std::memory_order_acquire))
        return false;

    // If the receiver cooperator is shutting down, stop accepting sends. Mark the
    // passage shut down as well so receiver-side Recv exits once drained.
    //
    if (m_target->IsShuttingDown())
    {
        state->m_shutdown.store(true, std::memory_order_release);
        return false;
    }

    if (!state->m_ring.Push(std::move(value)))
        return false;  // ring full

    // Submit failure can happen if target enters shutdown between push and submit.
    // Mark shutdown so future sends fail fast.
    //
    if (!SubmitWake(false))
    {
        state->m_shutdown.store(true, std::memory_order_release);
        return false;
    }

    // Push succeeded and wake is queued/in-flight. If shutdown starts now,
    // prevent additional sends; this item may still be delivered.
    //
    if (m_target->IsShuttingDown())
    {
        state->m_shutdown.store(true, std::memory_order_release);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Passage::TryRecv
// ---------------------------------------------------------------------------

template<typename T, size_t N>
bool Passage<T, N>::TryRecv(T& value)
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
// Passage::Drain
// ---------------------------------------------------------------------------

template<typename T, size_t N>
size_t Passage<T, N>::Drain(T* out, size_t maxCount)
{
    assert(Cooperator::thread_cooperator == m_target
           && "Passage::Drain must run on the target cooperator");
    if (!out || maxCount == 0) return 0;

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
// Passage::Recv
// ---------------------------------------------------------------------------
//
// The m_recv Coordinator invariant: held ↔ ring empty.
//   Constructor pre-acquires m_recv (empty ring). Wake lambda releases m_recv when
//   items arrive. Recv() re-acquires m_recv after consuming the last item.
//
// Phase 1 (fast): Pop directly from the MPSC ring. If ring now empty, re-acquire
//   m_recv immediately (not held at this point → Acquire is non-blocking).
//
// Phase 2 (yield loop): up to kYieldThreshold cooperative yields. The cooperator
//   calls Poll() after each resume, which may process the wake lambda's Submit CQE
//   and release m_recv before we fall through to the timed wait.
//
// Phase 3 (timed wait): CoordinateWithKill on m_recv with an adaptive timeout SQE
//   (10μs → 10ms, ×4 on miss, reset to 10μs on hit). When m_recv is released by
//   the wake lambda, we pop the item directly from the ring.
//

template<typename T, size_t N>
bool Passage<T, N>::Recv(T& value)
{
    assert(Cooperator::thread_cooperator == m_target
           && "Passage::Recv must run on the target cooperator");

    Context* ctx = Self();

    while (true)
    {
        // Phase 1: fast path — pop directly from the MPSC ring.
        //
        if (TryRecv(value))
            return true;

        if (m_state->m_shutdown.load(std::memory_order_acquire))
        {
            // Release m_recv on shutdown to propagate the closed signal.
            //
            if (m_state->m_recv.IsHeld())
                m_state->m_recv.Release(ctx);
            return false;
        }

        // Ring is empty. Ensure m_recv is held before entering the blocking phases
        // (invariant: held ↔ ring empty). Normally already held, but guard against
        // the case where the wake lambda released it just before we got here.
        //
        if (!m_state->m_recv.IsHeld())
            m_state->m_recv.Acquire(ctx);

        // Phase 2: yield, giving the cooperator a chance to process the wake Submit
        // CQE — which releases m_recv — before committing to a timeout SQE.
        //
        if (m_recvDryCount < m_tuning.yieldThreshold)
        {
            m_recvDryCount++;
            Yield();
            continue;
        }

        // Phase 3: timed CoordinateWithKill. Submits a timeout SQE alongside m_recv
        // so the cooperator wakes even if the wake lambda is delayed.
        //
        m_recvDryCount = 0;

        auto r = CoordinateWithKill(ctx, &m_state->m_recv,
                                    time::Interval(m_recvTimeoutUs));

        if (r.Killed())
            return false;

        if (r == &m_state->m_recv)
        {
            // m_recv acquired — wake lambda confirmed ring has items. Pop directly.
            //
            m_recvTimeoutUs = m_tuning.timeoutInitialUs;

            if (!m_state->m_ring.Pop(value))
            {
                // Shutdown released m_recv with an empty ring.
                //
                assert(m_state->m_shutdown.load());
                m_state->m_recv.Release(ctx);
                return false;
            }

            // If ring still has items, release m_recv (invariant: not held when non-empty).
            //
            if (!m_state->m_ring.IsEmpty())
                m_state->m_recv.Release(ctx);

            return true;
        }

        // Timed out: grow adaptively and retry.
        //
        m_recvTimeoutUs = std::min(
            m_recvTimeoutUs * static_cast<int64_t>(m_tuning.timeoutBackoff),
            m_tuning.timeoutMaxUs);
    }
}

// ---------------------------------------------------------------------------
// Passage::Shutdown
// ---------------------------------------------------------------------------

template<typename T, size_t N>
void Passage<T, N>::Shutdown()
{
    auto state = m_state;
    if (state->m_shutdown.exchange(true, std::memory_order_acq_rel))
        return;

    // On the target cooperator, release directly. Otherwise submit a wake
    // lambda to release on the target thread.
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

} // namespace chan
} // namespace coop
