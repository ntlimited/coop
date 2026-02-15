#include "sleep.h"
#include "ticker.h"

#include "coop/cooperator.h"
#include "coop/multi_coordinator.h"
#include "coop/self.h"

namespace coop
{

namespace time
{

Sleeper::Sleeper(Context* ctx, Ticker* ticker, Interval interval)
: m_coordinator(ctx)
, m_context(ctx)
, m_handle(interval, &m_coordinator)
, m_ticker(ticker)
{
}

Sleeper::~Sleeper()
{
    m_handle.Cancel();
}

Coordinator* Sleeper::GetCoordinator()
{
    return &m_coordinator;
}

void Sleeper::Submit()
{
    m_handle.Submit(m_ticker);
}

void Sleeper::Wait()
{
    m_handle.Wait(m_context);
}

void Sleeper::Sleep()
{
    Submit();
    Wait();
}

bool Sleep(Context* ctx, Interval interval)
{
    auto* ticker = ctx->GetCooperator()->GetTicker();
    assert(ticker);

    Sleeper sleeper(ctx, ticker, interval);
    sleeper.Submit();

    auto result = CoordinateWithKill(ctx, sleeper.GetCoordinator());
    return !result.Killed();
}

bool Sleep(Interval interval)
{
    return Sleep(Self(), interval);
}

} // end namespace coop::time
} // end namespace coop
