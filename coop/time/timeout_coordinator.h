#pragma once

#include "interval.h"

#include "coop/coordinator.h"
#include "coop/coordinator_extension.h"

namespace coop
{

struct Context;

namespace time
{

struct Driver;

struct TimeoutCoordinator : CoordinatorExtension
{
    TimeoutCoordinator(Interval interval, Coordinator* wrap);
    TimeoutCoordinator(TimeoutCoordinator const&) = delete;
    TimeoutCoordinator(TimeoutCoordinator&&) = delete;

    bool TryAcquire(Driver*, Context*);
    void Release(Context*);

  private:
    Interval m_interval;
    Coordinator* m_wrapped;
    Coordinator m_timeout;
};

} // end namespace coop::time
} // end namespace coop
