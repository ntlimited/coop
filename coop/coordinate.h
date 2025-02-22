#pragma once

#include "coordinator.h"
#include "coordinator_extension.h"

namespace coop
{

template<typename... Args>
struct MultiCoordinator;

template<>
struct MultiCoordinator<Coordinator*> : CoordinatorExtension
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

        coordinator->Acquire(context);
        if (HeldBy(coordinator, context))
        {
            return coordinator;
        }

        RemoveAsBlocked(coordinator, context);
        return nullptr;
    }

    Coordinator* coordinator;
};

template<typename... Args>
struct MultiCoordinator<Coordinator*, Args...> : MultiCoordinator<Args...>, CoordinatorExtension
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

        AddAsBlocked(coordinator, context);
        if (auto* held = Chained()(context, std::forward<Args>(args)...))
        {
            RemoveAsBlocked(coordinator, context);
            return held;
        }
        if (HeldBy(coordinator, context))
        {
            return coordinator;
        }
        RemoveAsBlocked(coordinator, context);
        return nullptr;
    }
};

template<typename... Args>
Coordinator* Coordinate(Context* ctx, Args... args)
{
    return MultiCoordinator<Args...>()(ctx, std::forward<Args>(args)...);
}

} // end namespace coop
