#pragma once

#include "handle.h"
#include "interval.h"

#include "coop/coordinator.h"

namespace coop
{

namespace time {

struct Ticker;

// The Sleeper is what actually does the work of sleeping, Sleep just packages it
//
struct Sleeper
{
    Sleeper(Context* ctx, Ticker* ticker, Interval interval);

    ~Sleeper();

    // TDOO this would be better composed in a design philosophy consistency kind of manner.
    // However, in practice wouldn't buy much anywhere this has been used so far.
    //
    Coordinator* GetCoordinator();

    void Submit();

    void Wait();

    void Sleep();

  private:
    Coordinator m_coordinator;
    
    Context*    m_context;
    Handle      m_handle;
    Ticker*     m_ticker;
    Interval    m_interval;
};

// TODO these should be kill-aware, but where exactly the kill-awareness comes from is debateable.
// It is probably fine to only do the kill awareness at a high level where the use of the API is
// inherently "I'm not sensitive enough to have an opinion".
//
// Come to think of it, this is probably related to the above comment regarding BYOC patterns;
// injecting the coordinator directly makes composing the top-level sleep pattern. "Signal using
// coordinator at the top level" is more natural that way imo (or at least my opinion right now)
//
bool Sleep(Context* ctx, Interval interval);

bool Sleep(Interval interval);

} // end namespace time
} // end namespace oop
