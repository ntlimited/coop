#include "shutdown_on_kill.h"

#include <utility>

#include "descriptor.h"
#include "shutdown.h"

#include "coop/cooperator.h"
#include "coop/coordinate_with.h"

namespace coop
{
namespace io
{

ShutdownOnKillGuard::ShutdownOnKillGuard(Context* owner, Descriptor& desc, int how)
: m_owner(owner)
, m_desc(&desc)
, m_ownerSignal(owner->GetKilledSignal())
, m_stop(owner)
, m_how(how)
, m_done(false)
{
    bool spawned = owner->GetCooperator()->Spawn([this](Context* watcher)
    {
        watcher->SetName("ShutdownOnKill");
        watcher->Detach();

        auto result = CoordinateWith(watcher, m_ownerSignal, &m_stop);
        if (result.index == 0 && m_desc->m_fd >= 0)
        {
            std::ignore = Shutdown(*m_desc, m_how);
        }

        m_done = true;
    }, &m_watcher);

    if (!spawned)
    {
        m_done = true;
    }
}

ShutdownOnKillGuard::~ShutdownOnKillGuard()
{
    if (!m_watcher || m_done)
    {
        return;
    }

    if (!m_owner->IsKilled())
    {
        m_stop.Notify(m_owner, false);
    }

    while (!m_done)
    {
        m_owner->Yield(true);
    }
}

} // end namespace coop::io
} // end namespace coop
