#pragma once

#include <utility>

#include "channel.h"
#include "coordinate_with.h"

// Select across channels using CoordinateWith under the hood.
//
// Recv cases (On) and send cases (OnSend) can be freely mixed in a single Select call.
// The underlying CoordinateWith fires on whichever coordinator becomes available first:
// m_recv not held ↔ channel non-empty (recv ready), m_send not held ↔ channel non-full
// (send ready).
//
//   // Loop until any channel shuts down:
//   while (coop::Select(ctx,
//       coop::On(ch1, [&](int v) { use(v); }),
//       coop::On(ch2, [&](int v) { use(v); })
//   )) {}
//
//   // Mixed recv + send select:
//   coop::Select(ctx,
//       coop::On(recvCh,  [&](int v) { use(v); }),
//       coop::OnSend(sendCh, value, [&]{ sent(); })
//   );
//
//   // Loop until all channels shut down (shutdown callback marks each done):
//   auto on1 = coop::On(ch1, [&](int v) { use(v); }, [&]{ done1 = true; });
//   auto on2 = coop::On(ch2, [&](int v) { use(v); }, [&]{ done2 = true; });
//   while (!done1 || !done2) {
//       if      (!done1 && !done2) coop::Select(ctx, on1, on2);
//       else if (!done1)           coop::Select(ctx, on1);
//       else                       coop::Select(ctx, on2);
//   }
//
// Select returns true if the selected case completed its operation (recv'd or sent),
// false if the selected channel was shut down (shutdown handler called, if any).
// SelectWithKill additionally returns false on kill; call IsKilled() to distinguish.
//

namespace coop
{

// ---------------------------------------------------------------------------
// Internal no-ops used as default send/shutdown handlers
// ---------------------------------------------------------------------------

struct NoShutdown
{
    void operator()() const {}
};

struct NoSent
{
    void operator()() const {}
};

// ---------------------------------------------------------------------------
// RecvCase — produced by On()
// ---------------------------------------------------------------------------

template<typename T, typename OnValue, typename OnShutdown>
struct RecvCase
{
    RecvChannel<T>& ch;
    OnValue   onValue;
    OnShutdown onShutdown;

    Coordinator* Coord() { return &ch.m_recv; }

    bool Fire()
    {
        T v;
        if (!ch.RecvAcquired(v))
        {
            onShutdown();
            return false;
        }
        onValue(std::move(v));
        return true;
    }
};

// ---------------------------------------------------------------------------
// RecvCase<void> — produced by On() for Channel<void>; callback takes no value
// ---------------------------------------------------------------------------

template<typename OnValue, typename OnShutdown>
struct RecvCase<void, OnValue, OnShutdown>
{
    Channel<void>& ch;
    OnValue    onValue;
    OnShutdown onShutdown;

    Coordinator* Coord() { return &ch.m_recv; }

    bool Fire()
    {
        if (!ch.RecvAcquired())
        {
            onShutdown();
            return false;
        }
        onValue();
        return true;
    }
};

// ---------------------------------------------------------------------------
// SendCase — produced by OnSend()
// ---------------------------------------------------------------------------

template<typename T, typename OnSent, typename OnShutdown>
struct SendCase
{
    SendChannel<T>& ch;
    T value;
    OnSent     onSent;
    OnShutdown onShutdown;

    Coordinator* Coord() { return &ch.m_send; }

    bool Fire()
    {
        if (!ch.SendAcquired(std::move(value)))
        {
            onShutdown();
            return false;
        }
        onSent();
        return true;
    }
};

// ---------------------------------------------------------------------------
// SendCase<void> — produced by OnSend() for Channel<void>; no value to send
// ---------------------------------------------------------------------------

template<typename OnSent, typename OnShutdown>
struct SendCase<void, OnSent, OnShutdown>
{
    Channel<void>& ch;
    OnSent     onSent;
    OnShutdown onShutdown;

    Coordinator* Coord() { return &ch.m_send; }

    bool Fire()
    {
        if (!ch.SendAcquired())
        {
            onShutdown();
            return false;
        }
        onSent();
        return true;
    }
};

// ---------------------------------------------------------------------------
// On — create a recv case (shutdown callback is optional)
// ---------------------------------------------------------------------------

template<typename T, typename OnValue>
auto On(RecvChannel<T>& ch, OnValue&& onValue)
    -> RecvCase<T, std::decay_t<OnValue>, NoShutdown>
{
    return { ch, std::forward<OnValue>(onValue), NoShutdown{} };
}

template<typename T, typename OnValue, typename OnShutdown>
auto On(RecvChannel<T>& ch, OnValue&& onValue, OnShutdown&& onShutdown)
    -> RecvCase<T, std::decay_t<OnValue>, std::decay_t<OnShutdown>>
{
    return { ch, std::forward<OnValue>(onValue), std::forward<OnShutdown>(onShutdown) };
}

template<typename OnValue>
auto On(Channel<void>& ch, OnValue&& onValue)
    -> RecvCase<void, std::decay_t<OnValue>, NoShutdown>
{
    return { ch, std::forward<OnValue>(onValue), NoShutdown{} };
}

template<typename OnValue, typename OnShutdown>
auto On(Channel<void>& ch, OnValue&& onValue, OnShutdown&& onShutdown)
    -> RecvCase<void, std::decay_t<OnValue>, std::decay_t<OnShutdown>>
{
    return { ch, std::forward<OnValue>(onValue), std::forward<OnShutdown>(onShutdown) };
}

// ---------------------------------------------------------------------------
// OnSend — create a send case (sent/shutdown callbacks are optional)
// ---------------------------------------------------------------------------

template<typename T>
auto OnSend(SendChannel<T>& ch, T value)
    -> SendCase<T, NoSent, NoShutdown>
{
    return { ch, std::move(value), NoSent{}, NoShutdown{} };
}

template<typename T, typename OnSent>
auto OnSend(SendChannel<T>& ch, T value, OnSent&& onSent)
    -> SendCase<T, std::decay_t<OnSent>, NoShutdown>
{
    return { ch, std::move(value), std::forward<OnSent>(onSent), NoShutdown{} };
}

template<typename T, typename OnSent, typename OnShutdown>
auto OnSend(SendChannel<T>& ch, T value, OnSent&& onSent, OnShutdown&& onShutdown)
    -> SendCase<T, std::decay_t<OnSent>, std::decay_t<OnShutdown>>
{
    return { ch, std::move(value), std::forward<OnSent>(onSent),
             std::forward<OnShutdown>(onShutdown) };
}

auto OnSend(Channel<void>& ch)
    -> SendCase<void, NoSent, NoShutdown>
{
    return { ch, NoSent{}, NoShutdown{} };
}

template<typename OnSent>
auto OnSend(Channel<void>& ch, OnSent&& onSent)
    -> SendCase<void, std::decay_t<OnSent>, NoShutdown>
{
    return { ch, std::forward<OnSent>(onSent), NoShutdown{} };
}

template<typename OnSent, typename OnShutdown>
auto OnSend(Channel<void>& ch, OnSent&& onSent, OnShutdown&& onShutdown)
    -> SendCase<void, std::decay_t<OnSent>, std::decay_t<OnShutdown>>
{
    return { ch, std::forward<OnSent>(onSent), std::forward<OnShutdown>(onShutdown) };
}

// ---------------------------------------------------------------------------
// Select — block until one case fires
// ---------------------------------------------------------------------------

template<typename... Cases>
bool Select(Context* ctx, Cases&&... cases)
{
    return std::apply(
        [ctx](auto&&... cs)
        {
            auto result = CoordinateWith(ctx, cs.Coord()...);
            bool ok = false;
            ((result == cs.Coord() ? (ok = cs.Fire(), true) : false) || ...);
            return ok;
        },
        std::forward_as_tuple(std::forward<Cases>(cases)...)
    );
}

// ---------------------------------------------------------------------------
// SelectWithKill — kill-aware Select
// ---------------------------------------------------------------------------

template<typename... Cases>
bool SelectWithKill(Context* ctx, Cases&&... cases)
{
    return std::apply(
        [ctx](auto&&... cs)
        {
            auto result = CoordinateWithKill(ctx, cs.Coord()...);
            if (result.Killed()) return false;
            bool ok = false;
            ((result == cs.Coord() ? (ok = cs.Fire(), true) : false) || ...);
            return ok;
        },
        std::forward_as_tuple(std::forward<Cases>(cases)...)
    );
}

// ---------------------------------------------------------------------------
// SelectAny — homogeneous recv from whichever of N same-typed channels fires
//
// Simpler than Select when you don't care which channel the value came from:
//
//   int v;
//   while (coop::SelectAny(&v, ch1, ch2, ch3)) { use(v); }
//
// Returns true if a value was received, false if the fired channel shut down.
// ---------------------------------------------------------------------------

template<typename T, typename... Channels>
bool SelectAny(T* out, Channels&... channels)
{
    Context* ctx = Self();
    auto result = CoordinateWith(ctx, (&channels.m_recv)...);
    bool ok = false;
    ((result == &channels.m_recv ? (ok = channels.RecvAcquired(*out), true) : false) || ...);
    return ok;
}

template<typename T, typename... Channels>
bool SelectAnyWithKill(T* out, Channels&... channels)
{
    Context* ctx = Self();
    auto result = CoordinateWithKill(ctx, (&channels.m_recv)...);
    if (result.Killed()) return false;
    bool ok = false;
    ((result == &channels.m_recv ? (ok = channels.RecvAcquired(*out), true) : false) || ...);
    return ok;
}

// ---------------------------------------------------------------------------
// SelectAnyVoid — homogeneous recv from whichever of N Channel<void> fires
//
//   while (coop::SelectAnyVoid(sig1, sig2)) { handle(); }
//
// Returns true if a signal was received, false if the fired channel shut down.
// ---------------------------------------------------------------------------

template<typename... Channels>
bool SelectAnyVoid(Channels&... channels)
{
    Context* ctx = Self();
    auto result = CoordinateWith(ctx, (&channels.m_recv)...);
    bool ok = false;
    ((result == &channels.m_recv ? (ok = channels.RecvAcquired(), true) : false) || ...);
    return ok;
}

template<typename... Channels>
bool SelectAnyVoidWithKill(Channels&... channels)
{
    Context* ctx = Self();
    auto result = CoordinateWithKill(ctx, (&channels.m_recv)...);
    if (result.Killed()) return false;
    bool ok = false;
    ((result == &channels.m_recv ? (ok = channels.RecvAcquired(), true) : false) || ...);
    return ok;
}

} // namespace coop
