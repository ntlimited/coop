#pragma once

#include <utility>

#include "coop/coordinator.h"
#include "coop/coordinator_extension.h"
#include "coop/self.h"

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

} // end coop::detail

// CoordinateWith is available in various flavors of syntactic sugar and is a fairly thin wrapper
// over the MultiCoordinator type which will probably get extended in the future. It (and the
// MultiCoordinator) is built for multiple highly sugared patterns, such as:
//
//  auto result = CoordinateWithKill(&coord);
//  if (result.Killed())
//  {
//      // We were killed
//  }
//
// Coordinator pointer comparison:
//
//  if (coord == CoordinateWithKill(&coord))
//  {
//      ...
//  }
//
// and Golang-style switches based on indices in the argument order:
//
//  switch (CoordinateWithKill(&timeoutCond, &lockCond))
//  {
//      case 0:
//          // Timeout cond triggered first
//          //
//          ...
//          break;
//      case 1:
//          // We received lockCond prior to the timeout triggering
//          //
//          ...
//          break;
//      default:
//          // Context has been killed
//          ...
//  }
//
// The contract is, very specifically, that:
//
//      exactly one coordinator will have been taken ownership
//      of when CoordinateWith returns
//
// This means that if we managed to acquire multiple coordinators (possible due to the way that
// scheduling works) we will have released the others before we return and they will need to be
// reacquired. The selection policy is "leftmost wins", which is why good hygeine is to put the
// kill flag first (and is what the `WithKill` flavors do).
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

template<typename... Args>
detail::AmbiResult CoordinateWith(Context* ctx, Args... args)
{
    MultiCoordinator<Args...> mc(std::forward<Args>(args)...);
    return mc.Acquire(ctx);
}

template<typename... Args>
detail::AmbiResult CoordinateWith(Args... args)
{
    return CoordinateWith(Self(), std::forward<Args>(args)...);
}

template<typename... Args>
detail::AmbiResult CoordinateWithKill(Context* ctx, Args... args)
{
    auto* signal = ctx->GetKilledSignal();
    auto result = CoordinateWith(ctx, signal->AsCoordinator(),
        std::forward<Args>(args)...);

    // If the kill signal won, reset its coordinator. MultiCoordinator's TryAcquire sets m_heldBy,
    // which would re-arm the coordinator and deadlock future uses.
    //
    if (result.index == 0)
    {
        signal->ResetCoordinator();
        return detail::AmbiResult { static_cast<size_t>(-1), nullptr };
    }

    result.index -= 1;
    return result;
}

template<typename... Args>
detail::AmbiResult CoordinateWithKill(Args... args)
{
    return CoordinateWithKill(Self(), std::forward<Args>(args)...);
}

} // end namespace coop
