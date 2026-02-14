#pragma once

namespace coop
{

struct CooperatorConfiguration
{
    int uringEntries;
    int tickerResolution;
};

static const CooperatorConfiguration s_defaultCooperatorConfiguration = {
    .uringEntries = 64,
    .tickerResolution = 3,
};

} // end namespace coop
