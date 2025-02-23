#pragma once

#include "coordinator.h"
#include "context.h"
#include "coordinator_extension.h"

namespace coop
{

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

        Coordinate ord(context);
        CoordinatorExtension().AddAsBlocked(coordinator, &ord);
        CoordinatorExtension().Block(context);

        if (CoordinatorExtension().HeldBy(coordinator, context))
        {
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

        Coordinate ord(context);

        CoordinatorExtension().AddAsBlocked(coordinator, &ord);

        // This is where we actually end up blocking for a coordinator
        //
        auto* held = Chained()(context, std::forward<Args>(args)...);

        // Note that we can end up holding multiple coordinators if we do not immediately resume
        // execution after the first is taken by us (e.g., Release with schedule = false). In that
        // case, choose only one to surface, currently prioritizing the first passed into the
        // method.
        //
        auto* here = CoordinatorExtension().HeldBy(coordinator, context);

        if (held)
        {
            if (here)
            {
                held->Release(context);
            }
            return here;
        }

        if (here)
        {
            return coordinator;
        }

        CoordinatorExtension().RemoveAsBlocked(coordinator, &ord);
        return nullptr;
    }
};

template<typename... Args>
Coordinator* Coordinate(Context* ctx, Args... args)
{
    return MultiCoordinator<Args...>()(ctx, std::forward<Args>(args)...);
}

} // end namespace coop
