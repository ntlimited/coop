#pragma once

#include "io/uring_configuration.h"

namespace coop
{

struct CooperatorConfiguration
{
    io::UringConfiguration uring;
    const char* name = nullptr;
};

static const CooperatorConfiguration s_defaultCooperatorConfiguration = {
    .uring = io::s_defaultUringConfiguration,
    .name = nullptr,
};

} // end namespace coop
