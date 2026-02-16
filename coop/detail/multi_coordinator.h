#pragma once

#include "coop/coordination_result.h"
#include "coop/coordinator.h"
#include "coop/detail/coordinator_extension.h"

namespace coop
{
namespace detail
{

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

    CoordinationResult Acquire(Context* ctx)
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
            return CoordinationResult{ idx, m_underlying[idx] };
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
            return CoordinationResult { idx, m_underlying[idx] };
        }
        assert(false);
        return CoordinationResult { C , nullptr };
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

} // end namespace detail
} // end namespace coop
