#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

#include "coop/multi_coordinator.h"
#include "coop/self.h"
#include "coop/io/handle.h"
#include "coop/io/timeout.h"
#include "coop/time/interval.h"

namespace coop
{

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

} // end coop::detail

// CoordinateWith blocks the calling context until one of the given coordinators is released. It
// always includes the context's kill signal; result.Killed() indicates that the context was killed.
// An optional time::Interval timeout can be supplied as the last argument (after the coordinator
// pointers); if the timeout fires, result.TimedOut() returns true.
//
// Basic usage:
//
//  auto result = CoordinateWith(&coord);
//  if (result.Killed())
//  {
//      // We were killed
//  }
//
// Coordinator pointer comparison:
//
//  if (coord == CoordinateWith(&coord))
//  {
//      ...
//  }
//
// With a timeout:
//
//  auto result = CoordinateWith(&coord, std::chrono::milliseconds(1000));  // timeout is always last
//  if (result.TimedOut())
//  {
//      ...
//  }
//
// Golang-style switch over indices in the argument order:
//
//  switch (CoordinateWith(&timeoutCond, &lockCond))
//  {
//      case 0:
//          // timeoutCond triggered first
//          break;
//      case 1:
//          // lockCond triggered first
//          break;
//      default:
//          // Context has been killed
//  }
//
// The contract is, very specifically, that:
//
//      exactly one coordinator will have been taken ownership
//      of when CoordinateWith returns
//
// This means that if we managed to acquire multiple coordinators (possible due to the way that
// scheduling works) we will have released the others before we return and they will need to be
// reacquired. The selection policy is "leftmost wins" among user-supplied coordinators; kill always
// takes priority over everything, and user coordinators take priority over the timeout.
//
// For direct access to the multi-coordinator mechanism without built-in kill or timeout handling,
// use MultiCoordinator::Acquire directly.
//

namespace detail
{

// Implementation for the non-timeout path. Kill signal is prepended at highest priority.
//
template<typename... Coords>
AmbiResult CoordinateWithImpl(Context* ctx, Coords... coords)
{
    auto* signal = ctx->GetKilledSignal();
    MultiCoordinator<Coordinator*, Coords...> mc(
        signal->AsCoordinator(), std::forward<Coords>(coords)...);
    auto result = mc.Acquire(ctx);

    if (result.index == 0)
    {
        signal->ResetCoordinator();
        return AmbiResult { static_cast<size_t>(-1), nullptr };
    }

    result.index -= 1;
    return result;
}

// Implementation for the timeout path. Internal ordering is [kill, coords..., timeout] so user
// coordinators take priority over the timeout in "leftmost wins" semantics. The timeout is
// submitted as an IORING_OP_TIMEOUT via the cooperator's io_uring ring; the io::Handle destructor
// cancels the pending timeout if it hasn't fired.
//
template<typename... Coords>
AmbiResult CoordinateWithTimeoutImpl(Context* ctx, time::Interval timeout, Coords... coords)
{
    auto* signal = ctx->GetKilledSignal();

    Coordinator timeoutCoord;
    io::Handle timeoutHandle(ctx, GetUring(), &timeoutCoord);
    if (!io::Timeout(timeoutHandle, timeout))
    {
        return AmbiResult { static_cast<size_t>(-3), nullptr };
    }

    // Order: [kill, user_coord1, ..., timeout]
    //
    MultiCoordinator<Coordinator*, Coords..., Coordinator*> mc(
        signal->AsCoordinator(), std::forward<Coords>(coords)..., &timeoutCoord);
    auto result = mc.Acquire(ctx);

    // io::Handle destructor cancels the pending timeout if it hasn't fired yet

    if (result.index == 0)
    {
        signal->ResetCoordinator();
        return AmbiResult { static_cast<size_t>(-1), nullptr };
    }

    constexpr size_t timeoutIdx = sizeof...(Coords) + 1;
    if (result.index == timeoutIdx)
    {
        return AmbiResult { static_cast<size_t>(-2), nullptr };
    }

    result.index -= 1;
    return result;
}

// Unpacks a tuple of [Coordinator*..., time::Interval] into the timeout implementation, using
// an index sequence to forward just the coordinator arguments.
//
template<size_t... Is, typename Tuple>
AmbiResult CoordinateWithTimeoutUnpack(Context* ctx, std::index_sequence<Is...>, Tuple& t)
{
    return CoordinateWithTimeoutImpl(
        ctx, std::get<std::tuple_size_v<Tuple> - 1>(t), std::get<Is>(t)...);
}

} // end namespace detail

// CoordinateWith: single entry point. If the last argument is a time::Interval it is interpreted
// as a timeout; all other arguments must be Coordinator pointers.
//
template<typename... Args>
detail::AmbiResult CoordinateWith(Context* ctx, Args... args)
{
    static_assert(sizeof...(Args) > 0,
        "CoordinateWith requires at least one argument.");

    if constexpr (std::is_convertible_v<detail::LastType<Args...>, time::Interval>)
    {
        static_assert(sizeof...(Args) > 1,
            "CoordinateWith with timeout requires at least one Coordinator.");
        auto t = std::make_tuple(args...);
        return detail::CoordinateWithTimeoutUnpack(
            ctx, std::make_index_sequence<sizeof...(Args) - 1>{}, t);
    }
    else
    {
        static_assert((std::is_same_v<Args, Coordinator*> && ...),
            "CoordinateWith arguments must be Coordinator pointers, "
            "optionally followed by a time::Interval timeout.");
        return detail::CoordinateWithImpl(ctx, args...);
    }
}

// Convenience: use Self() as the context
//
template<typename... Args>
detail::AmbiResult CoordinateWith(Args... args)
{
    return CoordinateWith(Self(), std::forward<Args>(args)...);
}

} // end namespace coop
