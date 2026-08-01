#pragma once

namespace coop
{

enum class SchedulerState
{
    // Between Context construction and first EnterContext. Launch<T> runs
    // T's constructor in this window; the context is in m_contexts but on
    // no scheduler list, and constructor code may itself Spawn (the
    // ShutdownOnKillGuard pattern), so accounting checks must treat
    // LAUNCHING as list-membership-exempt.
    LAUNCHING,
    YIELDED,
    BLOCKED,
    RUNNING,
};

} // end namespace coop
