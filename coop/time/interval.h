#pragma once

#include <chrono>

namespace coop
{

namespace time
{

// The interval typedef controls the precision with which the time system's API operates. Microseconds
// allow sub-millisecond timeouts to be expressed; actual delivery precision depends on the clock
// source and ticker resolution.
//
using Interval = std::chrono::microseconds;

} // end namespace coop::time
} // end namespace coop
