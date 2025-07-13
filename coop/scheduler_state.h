#pragma once

namespace coop
{

enum class SchedulerState
{
    YIELDED,
    BLOCKED,
    RUNNING,
};

} // end namespace coop
