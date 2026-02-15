#pragma once

#include "io/uring_configuration.h"

namespace coop
{

struct CooperatorConfiguration
{
    io::UringConfiguration uring;
};

static const CooperatorConfiguration s_defaultCooperatorConfiguration = {
    .uring = io::s_defaultUringConfiguration,
};

} // end namespace coop
