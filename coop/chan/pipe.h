#pragma once

#include <type_traits>
#include <utility>

#include "coop/chan/channel.h"
#include "coop/coordinate_with.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"

// Pipe spawns a context that reads from a source RecvChannel<T>, applies a transform
// function, and writes each result to an output channel owned by the returned PipeHandle.
//
// PipeHandle<Out, N> converts implicitly to Channel<Out>&. This is useful for explicit
// reference binding and for further Pipe stages:
//
//   auto doubled     = coop::chan::Pipe(ctx, intCh,   [](int v) { return v * 2; });
//   Channel<int>& ch = doubled;  // or: doubled.Chan()
//   auto stringified = coop::chan::Pipe(ctx, ch, [](int v) { return std::to_string(v); });
//
// For On() / Select(), use .Chan() or an explicit Channel<Out>& binding since template
// argument deduction does not consider user-defined conversions:
//
//   while (coop::chan::SelectWithKill(ctx,
//       coop::chan::On(stringified.Chan(), [&](std::string v) { s = std::move(v); })
//   )) { ... }
//
// Shutdown propagates: when the source shuts down, Recv returns false, the pipe context
// exits, and ~PipeHandle / Stop() shuts down the output channel, signalling downstream.
//
// To stop a pipe before its source shuts down, call Stop() (or shut down the source and
// let kill propagation reach the pipe context naturally via the parent-child tree).
//
// The output capacity N (default 1) decouples bursty producers and consumers.
//
// Note: PipeHandle is non-copyable and non-movable (Coordinator members). Pipe() returns
// a prvalue so C++17 mandatory copy elision (RVO) constructs the result directly in the
// caller's stack frame — no copy or move is ever performed.
//

namespace coop
{
namespace chan
{

// Forward declaration so detail::SpawnPipe can reference PipeHandle before it is defined.
//
template<typename Out, size_t N>
struct PipeHandle;

// ---------------------------------------------------------------------------
// detail::SpawnPipe — forward-declared so PipeHandle can friend it
// ---------------------------------------------------------------------------

namespace detail
{

template<size_t N, typename T, typename Out, typename Fn>
void SpawnPipe(PipeHandle<Out, N>& handle, RecvChannel<T>& src, Fn fn);

} // namespace detail

// ---------------------------------------------------------------------------
// PipeHandle
// ---------------------------------------------------------------------------

template<typename Out, size_t N>
struct PipeHandle
{
    PipeHandle(const PipeHandle&) = delete;
    PipeHandle(PipeHandle&&)      = delete;

    // Stop the pipe and wait for the pipe context to release all references to this
    // object, then shut down the output channel. Idempotent.
    //
    void Stop();

    ~PipeHandle() { Stop(); }

    // Implicit conversion to the output channel — useful for explicit Channel<Out>&
    // bindings and for passing to further Pipe() stages.
    //
    operator Channel<Out>&() { return m_ch; }

    // Explicit channel accessor.
    //
    Channel<Out>& Chan() { return m_ch; }

  private:
    // Constructed by the Pipe() factory via a prvalue return (mandatory RVO).
    // ctx initialises m_ch and m_stop (held, so the pipe blocks until Stop() releases it).
    //
    template<typename T, typename Fn>
    PipeHandle(Context* ctx, RecvChannel<T>& src, Fn fn)
    : m_ch(ctx)
    , m_stop(ctx)
    {
        detail::SpawnPipe<N>(*this, src, std::move(fn));
    }

    // Friend all SpawnPipe instantiations — SpawnPipe accesses m_exit, m_stop, and m_ch.
    //
    template<size_t N2, typename T2, typename Out2, typename Fn2>
    friend void detail::SpawnPipe(PipeHandle<Out2, N2>&, RecvChannel<T2>&, Fn2);

    template<size_t N2, typename T, typename Fn>
    friend auto Pipe(Context*, RecvChannel<T>&, Fn)
        -> PipeHandle<std::invoke_result_t<Fn, T>, N2>;

    FixedChannel<Out, N> m_ch;
    Coordinator          m_stop;  // starts held; Stop() releases to wake the pipe
    Coordinator          m_exit;  // held by pipe while running; Flash to wait for exit
};

// ---------------------------------------------------------------------------
// detail::SpawnPipe — separated so the lambda captures are self-contained
// ---------------------------------------------------------------------------

namespace detail
{

template<size_t N, typename T, typename Out, typename Fn>
void SpawnPipe(PipeHandle<Out, N>& handle, RecvChannel<T>& src, Fn fn)
{
    Spawn([&handle, &src, fn = std::move(fn)](Context* pipeCtx)
    {
        handle.m_exit.Acquire(pipeCtx);

        while (true)
        {
            // Wait for: kill signal | explicit stop | data available on source.
            //
            auto r = CoordinateWithKill(pipeCtx, &handle.m_stop, &src.m_recv);
            if (r.Killed() || r == &handle.m_stop) break;

            // src.m_recv has been acquired — complete the recv.
            //
            T val{};
            if (!src.RecvAcquired(val)) break;

            // Transform and forward. Blocks if the output channel is full.
            // Note: if the output is permanently full (no consumer), Stop() will block.
            //
            if (!handle.m_ch.Send(std::invoke(fn, std::move(val)))) break;
        }

        // Shut down the output channel so downstream consumers observe end-of-stream.
        // This covers both the natural exit (source shut down) and the kill path. Stop()
        // also calls Shutdown() but that runs after the consumer loop exits — so we must
        // shut down here to unblock any receiver that is currently blocked on m_ch.
        //
        handle.m_ch.Shutdown();

        handle.m_exit.Release(pipeCtx);
    });
    // Spawn immediately switches to the pipe context (EnterContext). By the time we
    // return, m_exit is held by the pipe and it is blocked in CoordinateWithKill.
    //
}

} // namespace detail

// ---------------------------------------------------------------------------
// Pipe factory — Out deduced from Fn's return type
// ---------------------------------------------------------------------------

template<size_t N = 1, typename T, typename Fn>
auto Pipe(Context* ctx, RecvChannel<T>& src, Fn fn)
    -> PipeHandle<std::invoke_result_t<Fn, T>, N>
{
    using Out = std::invoke_result_t<Fn, T>;
    return PipeHandle<Out, N>(ctx, src, std::move(fn));
    // Return is a prvalue: C++17 mandatory copy elision constructs PipeHandle directly
    // in the caller's frame — handle's address is stable before SpawnPipe captures it.
    //
}

// ---------------------------------------------------------------------------
// PipeHandle::Stop — defined here so Self() is in scope
// ---------------------------------------------------------------------------

template<typename Out, size_t N>
void PipeHandle<Out, N>::Stop()
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
