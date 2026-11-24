#pragma once

#include <cstddef>

// CMake and Bazel propagate one build-level value to the library and its consumers.
// An unmanaged build defaults to 16 KiB; define this macro consistently in every
// translation unit (65536 is the sanitizer default supplied by both build systems).
//
#ifndef COOP_DEFAULT_STACK_SIZE
#define COOP_DEFAULT_STACK_SIZE 16384
#endif

namespace coop
{

struct SpawnConfiguration
{
    int priority;
    size_t stackSize;

    // A daemon context (Grid stealer, background poller) is alive but does NOT count as
    // "real work" — Cooperator::NonDaemonContexts() excludes it, so a drain that waits
    // for outstanding work to finish is not held open by background helpers. Default
    // false; only daemon spawns touch the count, so non-daemon spawns pay nothing.
    //
    bool daemon = false;
};

static const SpawnConfiguration s_defaultConfiguration = {
    .priority = 0,
    .stackSize = COOP_DEFAULT_STACK_SIZE,
    .daemon = false,
};

} // end namespace coop
