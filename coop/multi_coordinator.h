#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

#include "coop/coordinator.h"
#include "coop/coordinator_extension.h"
#include "coop/self.h"
#include "coop/time/handle.h"

namespace coop
{

namespace detail
{
    // AmbiResult can be an index in the args (for switches) or a coordinator pointer for other
    // mechanics.
    //
    // No one is expected to ever store or use this type directly, instead it is expected to
    // immediately decay to one of its types.
    //
    struct AmbiResult
    {
        size_t          index;
        Coordinator*    coordinator;

        bool Killed() const { return index == static_cast<size_t>(-1); }

        bool TimedOut() const { return index == static_cast<size_t>(-2); }

        operator size_t() const
        {
            return index;
        }

        operator Coordinator*()
        {
            return coordinator;
        }

        Coordinator* operator->()
        {
            return coordinator;
        }
    };

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
//  auto result = CoordinateWith(&coord, time::Interval(1000));  // timeout is always last
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

// The MultiCoordinator wraps at least two coordinators and allows blocking until at least one
// has been given and exposes which one.
//
template<typename... Coordinators>
struct MultiCoordinator : CoordinatorExtension
{
    static constexpr size_t C = sizeof...(Coordinators);

    MultiCoordinator(Coordinators... coordinators)
    {
        Set(0, std::forward<Coordinators>(coordinators)...);
    }

    ~MultiCoordinator()
    {
    }

    detail::AmbiResult Acquire(Context* ctx)
    {
        size_t idx;
        for (idx = 0 ; idx < C ; idx++)
        {
            if (m_underlying[idx]->TryAcquire(ctx))
            {
                break;
            }
            SetContext(&m_coordinateds[idx], ctx);
            AddAsBlocked(m_underlying[idx], &m_coordinateds[idx]);
        }

        // If we are going to early exit, undo the hookups we did make.
        //
        if (idx < C)
        {
            for (size_t i = 0 ; i < idx ; i++)
            {
                RemoveAsBlocked(m_underlying[i], &m_coordinateds[i]);
            }

            SanityCheck();
            return detail::AmbiResult{ idx, m_underlying[idx] };
        }

        // We have hooked up all of the conditions to wait for any of them to wake us
        //
        Block(ctx);

        // Find which one(s) fired for us (in the event that we were unblocked but not immediately
        // scheduled).
        //
        for (idx = 0 ; idx < C ; idx++)
        {
            if (!m_coordinateds[idx].Satisfied())
            {
                RemoveAsBlocked(m_underlying[idx], &m_coordinateds[idx]);
                continue;
            }

            // Unlink and/or cleanup the rest of the Coordinators
            //
            for (size_t i = idx + 1 ; i < C ; i++)
            {
                if (m_coordinateds[i].Satisfied())
                {
                    // Our Coordinated instance was unlinked already
                    //
                    m_underlying[i]->Release(ctx, false);
                }
                else
                {
                    RemoveAsBlocked(m_underlying[i], &m_coordinateds[i]);
                }
            }
            SanityCheck();
            return detail::AmbiResult { idx, m_underlying[idx] };
        }
        assert(false);
        return detail::AmbiResult { C , nullptr };
    }

  private:
    template<typename... SetArgs>
    void Set(size_t idx, Coordinator* coord, SetArgs... args)
    {
        m_underlying[idx] = coord;
        Set(idx + 1, std::forward<SetArgs>(args)...);
    }

    void Set(size_t)
    {
    }

    void SanityCheck()
    {
        for (size_t i = 0 ; i < C ; i++)
        {
            assert(m_coordinateds[i].Disconnected());
        }
    }

    Coordinator* m_underlying[C];
    Coordinated m_coordinateds[C];
};

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
// coordinators take priority over the timeout in "leftmost wins" semantics. The timeout must be
// non-zero.
//
template<typename... Coords>
AmbiResult CoordinateWithTimeoutImpl(Context* ctx, time::Interval timeout, Coords... coords)
{
    auto* signal = ctx->GetKilledSignal();
    auto* ticker = GetTicker();
    assert(ticker);

    Coordinator timeoutCoord(ctx);
    time::Handle timeoutHandle(timeout, &timeoutCoord);
    timeoutHandle.Submit(ticker);

    // Order: [kill, user_coord1, ..., timeout]
    //
    MultiCoordinator<Coordinator*, Coords..., Coordinator*> mc(
        signal->AsCoordinator(), std::forward<Coords>(coords)..., &timeoutCoord);
    auto result = mc.Acquire(ctx);

    timeoutHandle.Cancel();

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

    if constexpr (std::is_same_v<detail::LastType<Args...>, time::Interval>)
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
