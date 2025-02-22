#include "coop/time/handle.h"

#include "coop/time/driver.h"

namespace coop
{

namespace time
{

void Handle::Submit(Driver* driver)
{
    assert(!m_deadline);
    assert(!m_coordinator->IsHeld());
    driver->Accept(this);
}


void Handle::Wait(Context* ctx)
{
    GetCoordinator()->Acquire(ctx);
    GetCoordinator()->Release(ctx);
}

void Handle::Cancel(Context* ctx)
{    
    assert(m_deadline);

    // Remove ourselves from the list for whatever bucket we're in
    //
    Pop();
    m_coordinator->Release(ctx);
    m_deadline = 0;
}

void Handle::Deadline(Context* ctx)
{
    assert(m_deadline);
    m_deadline = 0;
    GetCoordinator()->Release(ctx);
}

size_t Handle::SetDeadline(size_t epoch)
{
    auto intervals = m_interval.count();
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
