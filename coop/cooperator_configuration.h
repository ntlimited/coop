#pragma once

#include <cstdint>
#include <cstring>

#include "io/uring_configuration.h"

namespace coop
{

static constexpr int COOPERATOR_NAME_MAX = 64;

// How a cooperator backs the deadlines of pure timers (Sleep, the Grid stealer's recheck park; IO
// operation timeouts are always exact and out of scope -- see docs/timer_wheel_001.md).
//
// KernelPerTimer is the default: each sleep arms its own IORING_OP_TIMEOUT, which the kernel backs
// with a distinct hrtimer. Proven and simple, but its kernel timer cost scales with the number of
// in-flight sleeps.
//
// UserspaceQueue keeps a per-cooperator userspace deadline structure and arms a single kernel timer
// for the nearest deadline, servicing every due sleep per wakeup. It collapses the kernel hrtimer
// churn at fan-out, but is the newer, less-proven-at-scale path, so it is opt-in and lands disabled
// by default.
//
// No auto-switch is provided; the modes are explicit. Choosing between them: a high count of
// concurrent timers whose timeouts are mostly NOT hit -- registered then cancelled before firing --
// strongly favors UserspaceQueue, which pays only a cheap insert and cancel for a never-fired timer.
// A small number of timers, or timers whose deadlines are usually reached, favor KernelPerTimer.
//
enum class TimerMode : uint8_t
{
    KernelPerTimer,
    UserspaceQueue,
};

struct CooperatorConfiguration
{
    io::UringConfiguration uring;
    char name[COOPERATOR_NAME_MAX] = {};

    // Set the cooperator name (copies into the fixed buffer).
    //
    CooperatorConfiguration& SetName(const char* n)
    {
        if (n)
        {
            strncpy(name, n, COOPERATOR_NAME_MAX - 1);
            name[COOPERATOR_NAME_MAX - 1] = '\0';
        }
        return *this;
    }

    // CPU core to pin this cooperator's thread to. -1 (default) means auto round-robin
    // across available cores. Values >= 0 pin to that specific logical core ID.
    //
    int cpuAffinity = -1;

    // Backing strategy for pure-timer deadlines. Defaults to the proven kernel-per-timer path; the
    // userspace deadline queue is opt-in (see TimerMode).
    //
    TimerMode timerMode = TimerMode::KernelPerTimer;
};

static const CooperatorConfiguration s_defaultCooperatorConfiguration = {
    .uring = io::s_defaultUringConfiguration,
    .name = {},
    .cpuAffinity = -1,
    .timerMode = TimerMode::KernelPerTimer,
};

} // end namespace coop
