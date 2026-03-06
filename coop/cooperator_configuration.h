#pragma once

#include <cstring>

#include "io/uring_configuration.h"

namespace coop
{

static constexpr int COOPERATOR_NAME_MAX = 64;

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
};

static const CooperatorConfiguration s_defaultCooperatorConfiguration = {
    .uring = io::s_defaultUringConfiguration,
    .name = {},
    .cpuAffinity = -1,
};

} // end namespace coop
