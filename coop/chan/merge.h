#pragma once

#include <type_traits>
#include <utility>

#include "coop/chan/channel.h"
#include "coop/coordinate_with.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"

// Merge combines two source channels into one output channel. Items arrive in the order
// they become available, regardless of which source produced them.
//
// Usage:
//
//   auto merged = coop::chan::Merge(ctx, chA, chB);
//   int v;
//   while (merged.Chan().Recv(v)) { ... }
//
// MergeHandle<T, N> converts implicitly to Channel<T>& for chaining with further Merge
// or Pipe stages:
//
//   Channel<int>& m1 = Merge(ctx, chA, chB);
//   auto m2 = Merge(ctx, m1, chC);          // chain for 3+ sources
//
// For Select() / On(), use .Chan() (template argument deduction does not consider
// user-defined conversions):
//
//   On(merged.Chan(), [&](int v) { ... })
//
// Shutdown propagates: when BOTH sources shut down the merger exits and shuts down the
// output channel, signalling downstream consumers. If one source shuts down first the
// merger transparently continues draining the other.
//
// The output capacity N (default 1) decouples bursty producers and consumers.
//
// Note: MergeHandle is non-copyable and non-movable (Coordinator members). Merge() returns
// a prvalue so C++17 mandatory copy elision constructs the result directly in the caller's
// stack frame.
//

namespace coop
{
namespace chan
{

template<typename T, size_t N>
struct MergeHandle;

namespace detail
{

template<size_t N, typename T>
void SpawnMerger(MergeHandle<T, N>& handle, RecvChannel<T>& src1, RecvChannel<T>& src2);

} // namespace detail

// ---------------------------------------------------------------------------
// MergeHandle
// ---------------------------------------------------------------------------

template<typename T, size_t N>
struct MergeHandle
{
    MergeHandle(const MergeHandle&) = delete;
    MergeHandle(MergeHandle&&)      = delete;

    // Stop the merge and wait for the merger context to release all references to this
    // object, then shut down the output channel. Idempotent.
    //
    void Stop();

    ~MergeHandle() { Stop(); }

    // Implicit conversion to the output channel.
    //
    operator Channel<T>&() { return m_ch; }

    // Explicit channel accessor.
    //
    Channel<T>& Chan() { return m_ch; }

  private:
    MergeHandle(Context* ctx, RecvChannel<T>& src1, RecvChannel<T>& src2)
    : m_ch(ctx)
    , m_stop(ctx)
    {
        detail::SpawnMerger<N>(*this, src1, src2);
    }

    template<size_t N2, typename T2>
    friend void detail::SpawnMerger(MergeHandle<T2, N2>&, RecvChannel<T2>&, RecvChannel<T2>&);

    template<size_t N2, typename U>
    friend auto Merge(Context*, RecvChannel<U>&, RecvChannel<U>&)
        -> MergeHandle<U, N2>;

    FixedChannel<T, N> m_ch;
    Coordinator        m_stop;  // starts held; Stop() releases to wake the merger
    Coordinator        m_exit;  // held by merger while running; Flash to wait for exit
};

// ---------------------------------------------------------------------------
// detail::SpawnMerger
// ---------------------------------------------------------------------------

namespace detail
{

template<size_t N, typename T>
void SpawnMerger(MergeHandle<T, N>& handle, RecvChannel<T>& src1, RecvChannel<T>& src2)
{
    Spawn([&handle, &src1, &src2](Context* mergerCtx)
    {
        handle.m_exit.Acquire(mergerCtx);

        bool alive1 = true;
        bool alive2 = true;

        while (alive1 || alive2)
        {
            // Select on both alive sources (and the stop signal / kill).
            //
            CoordinationResult r;

            if (alive1 && alive2)
            {
                r = CoordinateWithKill(mergerCtx, &handle.m_stop,
                                       &src1.m_recv, &src2.m_recv);
            }
            else if (alive1)
            {
                r = CoordinateWithKill(mergerCtx, &handle.m_stop, &src1.m_recv);
            }
            else
            {
                r = CoordinateWithKill(mergerCtx, &handle.m_stop, &src2.m_recv);
            }

            if (r.Killed() || r == &handle.m_stop)
            {
                break;
            }

            // Complete the recv on the winning source.
            //
            T val{};

            if (r == &src1.m_recv)
            {
                if (!src1.RecvAcquired(val)) { alive1 = false; continue; }
            }
            else
            {
                if (!src2.RecvAcquired(val)) { alive2 = false; continue; }
            }

            if (!handle.m_ch.Send(std::move(val)))
            {
                break;
            }
        }

        handle.m_ch.Shutdown();
        handle.m_exit.Release(mergerCtx);
    });
}

} // namespace detail

// ---------------------------------------------------------------------------
// Merge factory
// ---------------------------------------------------------------------------

template<size_t N = 1, typename T>
auto Merge(Context* ctx, RecvChannel<T>& src1, RecvChannel<T>& src2)
    -> MergeHandle<T, N>
{
    return MergeHandle<T, N>(ctx, src1, src2);
}

// ---------------------------------------------------------------------------
// MergeHandle::Stop
// ---------------------------------------------------------------------------

template<typename T, size_t N>
void MergeHandle<T, N>::Stop()
{
    if (m_exit.IsHeld())
    {
        m_stop.Release(Self());
        m_exit.Flash(Self());
    }
    m_ch.Shutdown();
}

} // namespace chan
} // namespace coop
