#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include "coop/chan/channel.h"
#include "coop/coordinate_with.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/self.h"
#include "coop/time/interval.h"

// Passage<T, N> is a thread-safe channel that bridges external threads (or other cooperators)
// to a designated receiver cooperator. The Send side is callable from any thread; the Recv
// side is a plain RecvChannel<T>& that composes naturally with Recv(), Select(), Pipe(),
// Filter(), Merge(), etc.
//
// Architecture:
//   External threads call Send() → items are buffered in a mutex-protected ring buffer.
//   Exactly one drain submission is outstanding at a time (m_drainPending flag). When a
//   drain runs on the target cooperator, it forwards buffered items into an internal
//   FixedChannel<T,N> that the consumer reads via Chan(). If the internal channel is full
//   (consumer is slow), the drain blocks cooperatively until space is available, providing
//   natural back-pressure without busy-waiting.
//
//   All shared state (ring buffer, internal channel) lives in a heap-allocated State struct
//   managed by a shared_ptr. Drain submissions capture a shared_ptr copy, so State outlives
//   the Passage handle and in-flight drains never access freed memory.
//
// Usage:
//   // Create on the receiver cooperator's context:
//   coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());
//
//   // From any thread (non-blocking; returns false when ring is full or shut down):
//   passage.Send(42);
//
//   // On the receiver cooperator — prefer Recv() over Chan().Recv() for cross-thread use:
//   //   Recv() uses an adaptive yield → timed-wait strategy that avoids holding the
//   //   cooperator inside a blocking io_uring wait when data arrives frequently. It yields
//   //   up to kYieldThreshold times (giving Poll() a chance to process the SubmissionDrainer
//   //   CQE), then falls back to CoordinateWithKill with an adaptive timeout SQE.
//   //
//   int v;
//   while (passage.Recv(v)) { ... }
//
//   // Chan() is still available for composition with Select, Pipe, Filter, Merge, etc.:
//   while (passage.Chan().Recv(v)) { ... }
//
// Back-pressure: Send() returns false immediately when the external ring holds N items.
//   The external ring and the internal channel each have capacity N, so the Passage can
//   buffer at most 2N items in flight.
//
// Shutdown: ~Passage() calls Shutdown(). After shutdown, future Sends return false and
//   Chan().Recv() drains the internal channel then returns false. Items still in the
//   external ring at shutdown time are dropped. Shutdown() is idempotent.
//
// Thread-safety: Send() is safe to call from any thread concurrently.
//   Chan() and Shutdown() must be called from the receiver cooperator's context only.
//
// Requirement: T must be default-constructible and move-assignable.
//
// Note: Passage is non-copyable and non-movable.
//

namespace coop
{
namespace chan
{

template<typename T, size_t N = 64>
struct Passage
{
    Passage(const Passage&) = delete;
    Passage(Passage&&)      = delete;

    // ctx:    the receiver cooperator's current context (for internal channel init).
    // target: the cooperator that runs the consumer — drain submissions are posted here.
    //         Typically: ctx->GetCooperator().
    //
    explicit Passage(Context* ctx, Cooperator* target)
    : m_state(std::make_shared<State>(ctx))
    , m_target(target)
    {}

    ~Passage() { Shutdown(); }

    // Thread-safe. Callable from any thread or cooperator.
    // Returns false if the Passage is shut down or the external ring is full.
    //
    bool Send(T value);

    // Receive one item from the Passage on the receiver cooperator.
    //
    // Preferred over Chan().Recv() for direct consumption: uses an adaptive yield →
    // timed-wait strategy to minimize latency on live cross-thread handoffs without
    // burning CPU on a hot-spin. Returns false when the Passage is shut down and the
    // internal channel is drained.
    //
    // Must be called from the receiver cooperator's context only.
    //
    bool Recv(T& value);

    // Returns the internal channel for use on the receiver cooperator.
    // Composes transparently with Recv(), Select(), Pipe(), Filter(), Merge(), etc.
    //
    RecvChannel<T>& Chan() { return m_state->m_ch; }

    // Implicit conversion — same convenience interface as PipeHandle / FilterHandle.
    //
    operator RecvChannel<T>&() { return m_state->m_ch; }

    // Shuts down the Passage. Idempotent. Must be called from the receiver cooperator's
    // context (or its destructor path). Items already in the internal channel are still
    // delivered; items remaining in the external ring are dropped.
    //
    void Shutdown();

  private:
    struct State
    {
        explicit State(Context* ctx) : m_ch(ctx) {}

        FixedChannel<T, N>  m_ch;
        std::mutex          m_mutex;
        T                   m_ring[N];
        size_t              m_head{0};
        size_t              m_tail{0};
        size_t              m_size{0};
        std::atomic<bool>   m_shutdown{false};
        std::atomic<bool>   m_drainPending{false};
    };

    std::shared_ptr<State>  m_state;
    Cooperator*             m_target;

    // Adaptive recv state — receiver cooperator only, not shared.
    //
    int     m_recvDryCount{0};
    int64_t m_recvTimeoutUs{10};
};

// ---------------------------------------------------------------------------
// Passage::Send
// ---------------------------------------------------------------------------

template<typename T, size_t N>
bool Passage<T, N>::Send(T value)
{
    {
        std::lock_guard<std::mutex> lock(m_state->m_mutex);
        if (m_state->m_shutdown.load(std::memory_order_relaxed) || m_state->m_size == N)
        {
            return false;
        }
        m_state->m_ring[m_state->m_tail] = std::move(value);
        m_state->m_tail = (m_state->m_tail + 1) % N;
        m_state->m_size++;
    }

    // Submit exactly one drain at a time. If drainPending is already true, the outstanding
    // drain will pick up this item in its loop. The CAS ensures only one context drains.
    //
    bool expected = false;
    if (m_state->m_drainPending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
    {
        // Capture shared_ptr by value: State (and the internal channel) outlives Passage
        // if a drain submission is still in flight when the Passage handle is destroyed.
        //
        auto state = m_state;
        m_target->Submit([state](Context*)
        {
            while (true)
            {
                T item;
                {
                    std::lock_guard<std::mutex> lock(state->m_mutex);
                    if (state->m_size == 0)
                    {
                        // Ring empty. Clear the flag while holding the lock so any Send()
                        // that arrives after the clear sees drainPending=false and schedules
                        // a fresh drain — no lost wakeup possible.
                        //
                        state->m_drainPending.store(false, std::memory_order_release);
                        return;
                    }
                    item = std::move(state->m_ring[state->m_head]);
                    state->m_head = (state->m_head + 1) % N;
                    state->m_size--;
                }

                // Forward to the internal channel. Blocks cooperatively if full — no lock
                // held, so concurrent Sends can still push to the ring while we wait.
                // Returns false on shutdown; drain exits without clearing drainPending
                // (safe: m_shutdown prevents further submissions).
                //
                if (!state->m_ch.Send(std::move(item))) return;
            }
        });
    }

    return true;
}

// ---------------------------------------------------------------------------
// Passage::Recv
// ---------------------------------------------------------------------------
//
// Adaptive yield → timed-wait recv for cross-thread use.
//
// Phase 1 (fast): TryRecv. Succeeds immediately when the drain has already pushed items
//   into the internal channel. Resets dry-count and timeout on each hit so we stay in
//   the fast path as long as data flows.
//
// Phase 2 (yield loop): up to kYieldThreshold yields without data. Each yield returns
//   control to the cooperator, which calls Poll() between resumes. Poll() can process
//   the SubmissionDrainer's eventfd CQE, unblock the drainer, and trigger DrainSubmissions
//   — often delivering the next item within 1-3 yields for live producers.
//
// Phase 3 (timed wait): after kYieldThreshold dry yields, fall back to CoordinateWithKill
//   on the internal channel's m_recv coordinator with an adaptive timeout SQE. The timeout
//   ensures the cooperator wakes even if the SubmissionDrainer CQE is delayed. Timeout
//   grows ×4 per miss (10μs → 10ms cap) and resets to 10μs on the next hit.
//

template<typename T, size_t N>
bool Passage<T, N>::Recv(T& value)
{
    static constexpr int     kYieldThreshold   = 8;
    static constexpr int64_t kTimeoutInitialUs = 10;
    static constexpr int64_t kTimeoutMaxUs     = 10000;

    while (true)
    {
        // Phase 1: fast path — item already in the internal channel.
        //
        if (m_state->m_ch.TryRecv(value))
        {
            m_recvDryCount  = 0;
            m_recvTimeoutUs = kTimeoutInitialUs;
            return true;
        }

        if (m_state->m_ch.IsShutdown())
        {
            return false;
        }

        // Phase 2: yield, giving the cooperator a chance to run Poll() and process
        // the SubmissionDrainer CQE that will deliver our item.
        //
        if (m_recvDryCount < kYieldThreshold)
        {
            m_recvDryCount++;
            Yield();
            continue;
        }

        // Phase 3: timed wait. Submits a timeout SQE alongside the recv coordinator
        // wait, so the cooperator wakes regardless of SubmissionDrainer timing.
        //
        m_recvDryCount = 0;

        auto r = CoordinateWithKill(
            Self(), &m_state->m_ch.m_recv, time::Interval(m_recvTimeoutUs));

        if (r.Killed())
        {
            return false;
        }

        if (r == &m_state->m_ch.m_recv)
        {
            // m_recv acquired: complete the recv and reset timeout for next time.
            //
            m_recvTimeoutUs = kTimeoutInitialUs;
            return m_state->m_ch.RecvAcquired(value);
        }

        // Timed out: grow the wait interval adaptively, then retry from the top.
        //
        m_recvTimeoutUs = std::min(m_recvTimeoutUs * 4, kTimeoutMaxUs);
    }
}

// ---------------------------------------------------------------------------
// Passage::Shutdown
// ---------------------------------------------------------------------------

template<typename T, size_t N>
void Passage<T, N>::Shutdown()
{
    m_state->m_shutdown.store(true, std::memory_order_release);
    m_state->m_ch.Shutdown();
}

} // namespace chan
} // namespace coop
