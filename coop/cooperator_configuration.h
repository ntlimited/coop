#pragma once

#include "io/uring_configuration.h"

namespace coop
{

struct CooperatorConfiguration
{
    io::UringConfiguration uring;
    int tickerResolution;
};

static const CooperatorConfiguration s_defaultCooperatorConfiguration = {
    .uring = io::s_defaultUringConfiguration,
    .tickerResolution = 3,
};

} // end namespace coop
