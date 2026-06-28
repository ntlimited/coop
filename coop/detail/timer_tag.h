#pragma once

#include <cstdint>

namespace coop
{

namespace detail
{

// io_uring userdata tags for a cooperator's single deadline timer (docs/timer_wheel_001.md).
//
// The CQE dispatcher (io::Handle::Callback) recognizes these before the Handle/ArmedHandle decode:
// bit 2 marks a deadline-timer CQE, and within that, bit 0 distinguishes the timeout's own expiry
// (kTimerTag) from the acknowledgement of an in-place IORING_TIMEOUT_UPDATE (kTimerAckTag). The
// values are small integer cookies, not pointers — the owning cooperator is the thread's current
// cooperator, so no pointer needs to be encoded. They are disjoint from real Handle userdata, which
// is an aligned pointer with bit 2 clear, and from ArmedHandle, which is marked by bit 1.
//
static constexpr uintptr_t kTimerTag    = 0x4;        // bit 2: deadline-timer expiry
static constexpr uintptr_t kTimerAckTag = 0x4 | 0x1;  // bit 2 | bit 0: update ack (ignored)

} // namespace detail
} // namespace coop
