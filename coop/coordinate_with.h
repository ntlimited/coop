#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

#include "coop/detail/multi_coordinator.h"
#include "coop/self.h"
#include "coop/io/handle.h"
#include "coop/io/timeout.h"
#include "coop/time/interval.h"

namespace coop
{

struct Signal;

namespace detail
{
    // Type-level helper: extracts the last type in a parameter pack.
    //
    template<typename T, typename... Ts>
    struct LastOf { using Type = typename LastOf<Ts...>::Type; };

    template<typename T>
    struct LastOf<T> { using Type = T; };

    template<typename... Ts>
    using LastType = typename LastOf<Ts...>::Type;

    // Maps any type to Coordinator* for MultiCoordinator template parameter expansion. Both
    // Coordinator* and Signal* arguments are converted to Coordinator* at runtime; this alias
    // provides the compile-time type mapping for the MultiCoordinator parameter pack.
    //
    template<typename>
    using AlwaysCoordinatorPtr = Coordinator*;

} // end coop::detail

// CoordinateWith blocks the calling context until one of the given coordinators or signals is
// released. Arguments may be Coordinator* or Signal* in any combination. Signal* arguments are
// converted to their internal coordinator via AsCoordinator(), and the winning Signal is
// automatically cleaned up via ResetCoordinator(). An optional time::Interval timeout can be
// supplied as the last argument; if the timeout fires, result.TimedOut() returns true.
//
// CoordinateWith does NOT implicitly include the kill signal. For kill-aware coordination, use
// CoordinateWithKill, which prepends the context's kill signal and translates index 0 to the
// Killed() sentinel.
//
// Basic usage:
//
//  auto result = CoordinateWith(&coord);
//  // result.index == 0, result.coordinator == &coord
//
// With multiple coordinators:
//
//  auto result = CoordinateWith(&coord1, &coord2);
//  if (result == coord1) { ... }
//
// With a Signal:
//
//  auto result = CoordinateWith(&signal, &coord);
//  // index 0 = signal, index 1 = coord
//
// With a timeout:
//
//  auto result = CoordinateWith(&coord, std::chrono::milliseconds(1000));
//  if (result.TimedOut()) { ... }
//
// Kill-aware (prefer CoordinateWithKill sugar):
//
//  auto result = CoordinateWithKill(&coord);
//  if (result.Killed()) { ... }
//
// The contract is, very specifically, that:
//
//      exactly one coordinator will have been taken ownership
//      of when CoordinateWith returns
//
// This means that if we managed to acquire multiple coordinators (possible due to the way that
// scheduling works) we will have released the others before we return and they will need to be
// reacquired. The selection policy is "leftmost wins" among user-supplied arguments; user
// arguments take priority over the timeout.
//
//

namespace detail
{

// Implementation for the non-timeout path. Accepts mixed Coordinator*/Signal* arguments.
// Signal* arguments are converted to their internal coordinator, and the winning Signal is
// cleaned up via ResetCoordinator() to prevent re-arming after MultiCoordinator's TryAcquire.
//
template<typename... Args>
CoordinationResult CoordinateWithImpl(Context* ctx, Args... args)
{
    auto toCoord = [](auto arg) -> Coordinator* {
        if constexpr (std::is_same_v<decltype(arg), Signal*>)
            return arg->AsCoordinator();
        else
            return arg;
    };

    if constexpr (sizeof...(Args) == 1)
    {
        // Single argument: skip MultiCoordinator. Acquire handles both the fast path
        // (TryAcquire succeeds) and slow path (block until released) internally.
        //
        Coordinator* coord = toCoord(args...);
        coord->Acquire(ctx);

        auto maybeReset = [](auto arg) {
            if constexpr (std::is_same_v<decltype(arg), Signal*>)
                arg->ResetCoordinator();
        };
        (maybeReset(args), ...);

        return CoordinationResult{0, coord};
    }
    else
    {
        MultiCoordinator<AlwaysCoordinatorPtr<Args>...> mc(toCoord(args)...);
        auto result = mc.Acquire(ctx);

        // Reset any Signal whose coordinator won
        //
        size_t idx = 0;
        auto maybeReset = [&](auto arg) {
            if constexpr (std::is_same_v<decltype(arg), Signal*>)
            {
                if (result.index == idx) arg->ResetCoordinator();
            }
            idx++;
        };
        (maybeReset(args), ...);

        return result;
    }
}

// Implementation for the timeout path. Timeout coordinator is appended so user arguments take
// priority in "leftmost wins" semantics. The timeout is submitted as an IORING_OP_TIMEOUT via
// the cooperator's io_uring ring; the io::Handle destructor cancels it if it hasn't fired.
//
template<typename... Args>
CoordinationResult CoordinateWithTimeoutImpl(Context* ctx, time::Interval timeout, Args... args)
{
    Coordinator timeoutCoord;
    io::Handle timeoutHandle(ctx, GetUring(), &timeoutCoord);
    if (!io::Timeout(timeoutHandle, timeout))
    {
        return CoordinationResult { static_cast<size_t>(-3), nullptr };
    }

    auto toCoord = [](auto arg) -> Coordinator* {
        if constexpr (std::is_same_v<decltype(arg), Signal*>)
            return arg->AsCoordinator();
        else
            return arg;
    };

    // Order: [user args..., timeout]
    //
    MultiCoordinator<AlwaysCoordinatorPtr<Args>..., Coordinator*> mc(
        toCoord(args)..., &timeoutCoord);
    auto result = mc.Acquire(ctx);

    // io::Handle destructor cancels the pending timeout if it hasn't fired yet

    // Reset any Signal whose coordinator won
    //
    size_t idx = 0;
    auto maybeReset = [&](auto arg) {
        if constexpr (std::is_same_v<decltype(arg), Signal*>)
        {
            if (result.index == idx) arg->ResetCoordinator();
        }
        idx++;
    };
    (maybeReset(args), ...);

    constexpr size_t timeoutIdx = sizeof...(Args);
    if (result.index == timeoutIdx)
    {
        return CoordinationResult { static_cast<size_t>(-2), nullptr };
    }

    return result;
}

// Unpacks a tuple of [args..., time::Interval] into the timeout implementation, using
// an index sequence to forward just the non-timeout arguments.
//
template<size_t... Is, typename Tuple>
CoordinationResult CoordinateWithTimeoutUnpack(Context* ctx, std::index_sequence<Is...>, Tuple& t)
{
    return CoordinateWithTimeoutImpl(
        ctx, std::get<std::tuple_size_v<Tuple> - 1>(t), std::get<Is>(t)...);
}

} // end namespace detail

// CoordinateWith: single entry point. If the last argument is a time::Interval it is interpreted
// as a timeout; all other arguments must be Coordinator* or Signal* pointers.
//
template<typename... Args>
CoordinationResult CoordinateWith(Context* ctx, Args... args)
{
    static_assert(sizeof...(Args) >= 1,
        "CoordinateWith requires at least one argument.");

    if constexpr (std::is_convertible_v<detail::LastType<Args...>, time::Interval>)
    {
        static_assert(sizeof...(Args) > 1,
            "CoordinateWith with timeout requires at least one Coordinator or Signal.");
        auto t = std::make_tuple(args...);
        return detail::CoordinateWithTimeoutUnpack(
            ctx, std::make_index_sequence<sizeof...(Args) - 1>{}, t);
    }
    else
    {
        static_assert(((std::is_same_v<Args, Coordinator*>
            || std::is_same_v<Args, Signal*>) && ...),
            "CoordinateWith arguments must be Coordinator* or Signal* pointers, "
            "optionally followed by a time::Interval timeout.");
        return detail::CoordinateWithImpl(ctx, args...);
    }
}

// Convenience: use Self() as the context
//
template<typename... Args>
CoordinationResult CoordinateWith(Args... args)
{
    return CoordinateWith(Self(), std::forward<Args>(args)...);
}

// CoordinateWithKill: kill-aware convenience wrapper. Prepends the context's kill signal as the
// first argument, delegates to CoordinateWith, and translates index 0 (kill) to the Killed()
// sentinel while shifting user indices down by 1.
//
template<typename... Args>
CoordinationResult CoordinateWithKill(Context* ctx, Args... args)
{
    // Fast path for the common case: single Coordinator*, not killed, uncontended.
    // Bypasses MultiCoordinator construction entirely — avoids 2 Coordinated objects,
    // 2 array stores, and the TryAcquire loop overhead. This is the IO hot path
    // (Handle::Wait calls CoordinateWithKill with a single Coordinator*).
    //
    // Safety: scheduling is cooperative, so nothing can change between IsKilled() and
    // TryAcquire(). If TryAcquire succeeds we never blocked, so the kill signal never
    // had a chance to fire. If TryAcquire fails, we fall through to the full
    // MultiCoordinator path which multiplexes the kill signal properly.
    //
    if constexpr (sizeof...(Args) == 1 && (std::is_same_v<Args, Coordinator*> && ...))
    {
        if (ctx->IsKilled())
        {
            return CoordinationResult { static_cast<size_t>(-1), nullptr };
        }

        Coordinator* coord = [](Coordinator* c) { return c; }(args...);
        if (coord->TryAcquire(ctx))
        {
            return CoordinationResult{0, coord};
        }
    }

    auto result = CoordinateWith(ctx, ctx->GetKilledSignal(), std::forward<Args>(args)...);

    // Kill signal fired (index 0)
    //
    if (result.index == 0)
    {
        return CoordinationResult { static_cast<size_t>(-1), nullptr };
    }

    // Sentinel passthrough (timeout, error)
    //
    if (result.coordinator == nullptr)
    {
        return result;
    }

    // Normal result — shift past the prepended kill signal
    //
    result.index -= 1;
    return result;
}

// Convenience: use Self() as the context
//
template<typename... Args>
CoordinationResult CoordinateWithKill(Args... args)
{
    return CoordinateWithKill(Self(), std::forward<Args>(args)...);
}

} // end namespace coop
