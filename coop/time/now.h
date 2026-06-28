#pragma once

#include <cstdint>
#include <ctime>

namespace coop
{

namespace time
{

// Absolute monotonic clock in microseconds, the same clock io_uring's relative timeouts count from
// (CLOCK_MONOTONIC). Timer deadlines are stored and compared in this unit so the userspace timer
// queue and the kernel timer it arms share a clock domain.
//
// Two roundings, each chosen to keep the one-sided "never fires early" covenant. The plain reader
// FLOORS, and is used where a smaller value is safe: the service gate (do not release a sleep until
// floor(now) has reached its deadline) and the kernel-timer arm (a slightly longer relative wait
// never wakes early). The Ceil reader ROUNDS UP, and is used to stamp a sleep's deadline at
// registration: flooring the start instead would place the absolute deadline up to one microsecond
// before now+interval in real terms, letting the sleep return a hair early. Rounding the start up
// costs at most one microsecond of over-sleep, which the covenant explicitly permits.
//
inline int64_t MonotonicMicros()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

inline int64_t MonotonicMicrosCeil()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + (ts.tv_nsec + 999) / 1000;
}

} // end namespace time
} // end namespace coop
