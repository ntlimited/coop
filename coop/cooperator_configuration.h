#pragma once

namespace coop
{

struct CooperatorConfiguration
{
    int uringEntries;
    int uringRegisteredSlots;
    int tickerResolution;
};

static const CooperatorConfiguration s_defaultCooperatorConfiguration = {
    .uringEntries = 64,
    .uringRegisteredSlots = 64,
    .tickerResolution = 3,
};

} // end namespace coop
