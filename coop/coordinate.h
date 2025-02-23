#pragma once

#include <utility>

#include "coordinator.h"
#include "context.h"
#include "coordinator_extension.h"
#include "self.h"

namespace coop
{

namespace detail
{

// MultiCoordinator is used to compose the functionality of Coordinate(...) and, like all detail
// members, should not be touched directly by users.
//
// TODO: would a more stateful version of this be useful? running this in a loop would be O(N)
// each time setting up and tearing down hookups
//
template<typename... Args>
struct MultiCoordinator;

template<>
struct MultiCoordinator<Coordinator*>
{
    MultiCoordinator()
    {
    }

    Coordinator* operator()(Context* context, Coordinator* coordinator)
    {
        if (coordinator->TryAcquire(context))
        {
            return coordinator;
        }

        Coordinated ord(context);
        CoordinatorExtension().AddAsBlocked(coordinator, &ord);
        CoordinatorExtension().Block(context);

        if (ord.Satisfied())
        {
            coordinator->m_blocking.Visit([&](Coordinated* other)
            {
                assert(&ord != other);
                return true;
            });
            return coordinator;
        }
    
        CoordinatorExtension().RemoveAsBlocked(coordinator, &ord);
        return nullptr;
    }

    Coordinator* coordinator;
};

template<typename... Args>
struct MultiCoordinator<Coordinator*, Args...> : MultiCoordinator<Args...>
{
    using Chained = MultiCoordinator<Args...>;

    MultiCoordinator()
    {
    }

    Coordinator* operator()(Context* context, Coordinator* coordinator, Args... args)
    {
        if (coordinator->TryAcquire(context))
        {
            return coordinator;
        }

        // CoordinatorExtension inherently means code is dangerous; in this case, beyond blocking
        // state considerations, the ord must be garbage collected from the blocklist in all cases.
        //
        Coordinated ord(context);
        CoordinatorExtension().AddAsBlocked(coordinator, &ord);

        // This is where we actually end up blocking for a coordinator
        //
        auto* held = Chained()(context, std::forward<Args>(args)...);

        if (held)
        {
            // Note that we can end up holding multiple coordinators if we do not immediately resume
            // execution after the first is taken by us (e.g., Release with schedule = false). In
            // that case, choose only one to surface, currently prioritizing the first passed into
            // the method.
            //
            if (ord.Satisfied())
            {
                held->Release(context);
                return coordinator;
            }
            CoordinatorExtension().RemoveAsBlocked(coordinator, &ord);
            return held;
        }

        if (ord.Satisfied())
        {
            return coordinator;
        }

        CoordinatorExtension().RemoveAsBlocked(coordinator, &ord);
        return nullptr;
    }
};

} // end namespace coop::detail

// CoordinateWithKill allows for Golang style switch statement waiting on multiple coordinators at
// once. The `WithKill` flavor automatically watches the kill flag for the active context, for
// patterns like:
//
//  switch (Coordinate(ctx, resourceCoord, timeoutCoord))
//  {
//      case resourceCoord:
//          ...
//          break;
//      case timeoutCoord:
//          ...
//          break;
//      default:
//          // Killed condition for the current context
//          return false;
//  }
//
template<typename... Args>
Coordinator* CoordinateWithKill(Context* ctx, Args... args)
{
    return Coordinate(ctx, ctx->GetKilledSignal(), std::forward<Args>(args)...);
}

template<typename... Args>
Coordinator* CoordinateWithKill(Args... args)
{
    return CoordinateWithKill(Self(), std::forward<Args>(args)...);
}

template<typename... Args>
Coordinator* Coordinate(Context* ctx, Args... args)
{
    return detail::MultiCoordinator<Args...>()(ctx, std::forward<Args>(args)...);
}

template<typename... Args>
Coordinator* Coordinate(Args... args)
{
    return Coordinate(Self(), std::forward<Args>(args)...);
}

} // end namespace coop
