#include "handle.h"

#include "ticker.h"

#include "coop/cooperator.h"
#include "coop/self.h"

namespace coop
{

namespace time
{

Handle::~Handle()
{
    // When the handle is being destroyed, it must not be in a ticker bucket or it will
    // explode.
    //
    if (m_deadline)
    {
        assert(!Disconnected());
        m_list->Remove(this);
    }

    assert(Disconnected());
}

void Handle::Submit(Ticker* ticker /* = nullptr */)
{
    if (!ticker)
    {
        ticker = Self()->GetCooperator()->GetTicker();
    }
    assert(!m_deadline);
    assert(Disconnected());
    ticker->Accept(this);
}


void Handle::Wait(Context* ctx)
{
    GetCoordinator()->Acquire(ctx);
}

void Handle::Cancel()
{
    if (!m_deadline)
    {
        assert(Disconnected());
        return;
    }
    
    assert(!Disconnected());

    // Remove ourselves from the list for whatever bucket we're in
    //
    m_list->Remove(this);
    m_deadline = 0;
}

void Handle::Deadline(Context* ctx)
{
    // Must have had deadline scheduled
    //
    assert(m_deadline);

    // Must have already been disconnected from the bucket holding the handle
    //
    assert(Disconnected());

    // Reset deadline as we no longer have one
    //
    m_deadline = 0;

    // Signal
    //
    GetCoordinator()->Release(ctx);
}

size_t Handle::SetDeadline(size_t epoch, int resolution)
{
    auto intervals = m_interval.count() >> resolution;
    m_deadline = epoch + intervals;
    return intervals;
}

size_t Handle::GetDeadline() const
{
    return m_deadline;
}

Coordinator* Handle::GetCoordinator()
{
    return m_coordinator;
}

} // end namespace coop::time
} // end namespace coop
