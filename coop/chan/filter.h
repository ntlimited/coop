#pragma once

#include <utility>

#include "coop/chan/channel.h"
#include "coop/coordinate_with.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"

// Filter spawns a context that reads from a source RecvChannel<T>, passes each item
// through a predicate, and forwards only the items that satisfy it to an output channel
// owned by the returned FilterHandle.
//
// FilterHandle<T, N> has the same interface as PipeHandle — it converts implicitly to
// Channel<T>& and composes transparently with further Pipe/Filter/Merge stages:
//
//   auto evens = coop::chan::Filter(ctx, intCh, [](int v) { return v % 2 == 0; });
//   auto big   = coop::chan::Filter(ctx, evens.Chan(), [](int v) { return v > 10; });
//
// Shutdown propagates: when the source shuts down, the filter exits and shuts down its
// output channel, signalling downstream consumers.
//
// The output capacity N (default 1) decouples bursty producers and consumers.
//
// Note: FilterHandle is non-copyable and non-movable. Filter() returns a prvalue so
// C++17 mandatory copy elision constructs the result directly in the caller's frame.
//

namespace coop
{
namespace chan
{

template<typename T, size_t N>
struct FilterHandle;

namespace detail
{

template<size_t N, typename T, typename Pred>
void SpawnFilter(FilterHandle<T, N>& handle, RecvChannel<T>& src, Pred pred);

} // namespace detail

// ---------------------------------------------------------------------------
// FilterHandle
// ---------------------------------------------------------------------------

template<typename T, size_t N>
struct FilterHandle
{
    FilterHandle(const FilterHandle&) = delete;
    FilterHandle(FilterHandle&&)      = delete;

    void Stop();

    ~FilterHandle() { Stop(); }

    operator Channel<T>&() { return m_ch; }

    Channel<T>& Chan() { return m_ch; }

  private:
    template<typename Pred>
    FilterHandle(Context* ctx, RecvChannel<T>& src, Pred pred)
    : m_ch(ctx)
    , m_stop(ctx)
    {
        detail::SpawnFilter<N>(*this, src, std::move(pred));
    }

    template<size_t N2, typename T2, typename Pred2>
    friend void detail::SpawnFilter(FilterHandle<T2, N2>&, RecvChannel<T2>&, Pred2);

    template<size_t N2, typename U, typename Pred>
    friend auto Filter(Context*, RecvChannel<U>&, Pred)
        -> FilterHandle<U, N2>;

    FixedChannel<T, N> m_ch;
    Coordinator        m_stop;
    Coordinator        m_exit;
};

// ---------------------------------------------------------------------------
// detail::SpawnFilter
// ---------------------------------------------------------------------------

namespace detail
{

template<size_t N, typename T, typename Pred>
void SpawnFilter(FilterHandle<T, N>& handle, RecvChannel<T>& src, Pred pred)
{
    Spawn([&handle, &src, pred = std::move(pred)](Context* filterCtx)
    {
        handle.m_exit.Acquire(filterCtx);

        while (true)
        {
            auto r = CoordinateWithKill(filterCtx, &handle.m_stop, &src.m_recv);
            if (r.Killed() || r == &handle.m_stop)
            {
                break;
            }

            T val{};
            if (!src.RecvAcquired(val))
            {
                break;
            }

            if (pred(val))
            {
                if (!handle.m_ch.Send(std::move(val)))
                {
                    break;
                }
            }
        }

        handle.m_ch.Shutdown();
        handle.m_exit.Release(filterCtx);
    });
}

} // namespace detail

// ---------------------------------------------------------------------------
// Filter factory
// ---------------------------------------------------------------------------

template<size_t N = 1, typename T, typename Pred>
auto Filter(Context* ctx, RecvChannel<T>& src, Pred pred)
    -> FilterHandle<T, N>
{
    return FilterHandle<T, N>(ctx, src, std::move(pred));
}

// ---------------------------------------------------------------------------
// FilterHandle::Stop
// ---------------------------------------------------------------------------

template<typename T, size_t N>
void FilterHandle<T, N>::Stop()
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
