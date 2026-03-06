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
};

static const CooperatorConfiguration s_defaultCooperatorConfiguration = {
    .uring = io::s_defaultUringConfiguration,
    .name = {},
};

} // end namespace coop
