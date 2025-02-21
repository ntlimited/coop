#pragma once

#include <cstddef>

namespace coop
{

struct SpawnConfiguration
{
	int priority;
	size_t stackSize;
};

static const SpawnConfiguration s_defaultConfiguration = {
	.priority = 0,
	.stackSize = 16384,
};

} // end namespace coop
