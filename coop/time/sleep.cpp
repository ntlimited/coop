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

bool Sleeper::Wait()
{
    auto result = CoordinateWith(m_context, &m_coordinator);
    return !result.Killed();
}

bool Sleeper::Sleep()
{
    Submit();
    return Wait();
}

bool Sleep(Context* ctx, Interval interval)
{
    auto* ticker = ctx->GetCooperator()->GetTicker();
    assert(ticker);

    return Sleeper(ctx, ticker, interval).Sleep();
}

bool Sleep(Interval interval)
{
    return Sleep(Self(), interval);
}

} // end namespace coop::time
} // end namespace coop
