#pragma once

#include "coop/time/interval.h"

namespace coop
{

namespace io
{

struct Handle;

// Submit a standalone IORING_OP_TIMEOUT. Converts the interval to a __kernel_timespec stored in
// the handle's m_timeout field, acquires the coordinator, and submits. The primary CQE fires with
// -ETIME when the timeout expires. This is the mechanism for all non-IO timeouts (sleep,
// CoordinateWith).
//
bool Timeout(Handle& handle, time::Interval interval);

} // end namespace coop::io
} // end namespace coop
