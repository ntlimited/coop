#pragma once

#include <cstdint>

#include "interval.h"

#include "coop/coordinator.h"
#include "coop/io/handle.h"

namespace coop
{

namespace time {

enum class SleepResult : int8_t
{
    Ok,
    Killed,
    Error
};

// The Sleeper is what actually does the work of sleeping, Sleep just packages it. Timeouts are
// dispatched via IORING_OP_TIMEOUT through the cooperator's io_uring ring.
//
struct Sleeper
{
    Sleeper(Context* ctx, Interval interval);

    ~Sleeper();

    Coordinator* GetCoordinator();

    // Returns false if the timeout SQE could not be submitted (ring full).
    //
    bool Submit();

    // Blocks until the timeout fires or the context is killed. Returns true if the sleep completed
    // normally, false if the context was killed. Only call after a successful Submit().
    //
    bool Wait();

    // Submit and wait. Returns Ok if completed, Killed if the context was killed, Error if the
    // timeout could not be submitted.
    //
    SleepResult Sleep();

  private:
    Coordinator m_coordinator;

    Context*    m_context;
    io::Handle  m_handle;
    Interval    m_interval;
};

// Kill-aware sleep. Returns Ok if the sleep completed normally, Killed if the context was killed
// before the interval elapsed, Error if the timeout could not be submitted.
//
SleepResult Sleep(Context* ctx, Interval interval);

SleepResult Sleep(Interval interval);

} // end namespace time
} // end namespace coop
