#pragma once

#include <chrono>

namespace coop
{

namespace time
{

// The interval typedef controls the precision with which the time system attempts to operate. This
// was intended to work in tandem with
//
using Interval = std::chrono::milliseconds;

} // end namespace coop::time
} // end namespace coop
