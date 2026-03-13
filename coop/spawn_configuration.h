#pragma once

#include <cstddef>

#ifndef COOP_DEFAULT_STACK_SIZE
#define COOP_DEFAULT_STACK_SIZE 16384
#endif

namespace coop
{

struct SpawnConfiguration
{
    int priority;
    size_t stackSize;
};

static const SpawnConfiguration s_defaultConfiguration = {
    .priority = 0,
    .stackSize = COOP_DEFAULT_STACK_SIZE,
};

} // end namespace coop
