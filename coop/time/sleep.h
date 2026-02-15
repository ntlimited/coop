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

    Coordinator* GetCoordinator();

    void Submit();

    // Returns true if the sleep completed normally, false if the context was killed.
    //
    bool Wait();

    // Submit and wait. Returns true if completed, false if killed.
    //
    bool Sleep();

  private:
    Coordinator m_coordinator;
    
    Context*    m_context;
    Handle      m_handle;
    Ticker*     m_ticker;
    Interval    m_interval;
};

// Kill-aware sleep. Returns true if the sleep completed normally, false if the context was killed
// before the interval elapsed.
//
bool Sleep(Context* ctx, Interval interval);

bool Sleep(Interval interval);

} // end namespace time
} // end namespace oop
