#pragma once

#include "interval.h"
#include "ticker.h"

#include "coop/self.h"

namespace coop
{

namespace time {

// The Sleeper is what actually does the work of sleeping, Sleep just packages it
//
struct Sleeper
{

    Sleeper(Context* ctx, Ticker* ticker, Interval interval)
    : m_coordinator(ctx)
    , m_context(ctx)
    , m_handle(interval, &m_coordinator)
    , m_ticker(ticker)
    {
    }

    ~Sleeper()
    {
        m_handle.Cancel();
    }

    Coordinator* GetCoordinator()
    {
        return &m_coordinator;
    }

    void Submit()
    {
        m_handle.Submit(m_ticker);
    }

    void Wait()
    {
        m_handle.Wait(m_context);
    }

    void Sleep()
    {
        Submit();
        Wait();
    }

  private:
    Coordinator m_coordinator;
    
    Context*    m_context;
    Handle      m_handle;
    Ticker*     m_ticker;
    Interval    m_interval;
};

bool Sleep(Context* ctx, Interval interval)
{
    auto* ticker = ctx->GetCooperator()->GetTicker();
    if (!ticker)
    {
        return false;
    }

    Sleeper(ctx, ticker, interval).Sleep();
    return true;
}

bool Sleep(Interval interval)
{
    return Sleep(Self(), interval);
}


} // end namespace time
} // end namespace oop
