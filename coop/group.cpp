#include "group.h"

#include "coop/coordinate_with.h"

namespace coop
{

void Group::OnChildDone(bool ok)
{
    // First failure fails the group and cancels the siblings. Killing runs over every
    // stored handle: already-finished children have stale handles that Kill treats as
    // no-ops (the ABA check in BoundarySafeKill), and killing this child's own handle is
    // benign since it is already exiting. Live siblings observe the kill through their
    // kill-aware waits and unwind.
    //
    if (!ok && !m_cancelling)
    {
        m_failed = true;
        m_cancelling = true;
        for (Context::Handle& handle : m_handles)
        {
            handle.Kill();
        }
    }

    m_live--;
    if (m_live == 0 && m_done.IsHeld())
    {
        // Wake the owner's Wait. schedule=false: the owner is resumed by the scheduler
        // loop, not switched into from a dying child.
        //
        m_done.Release(nullptr, false);
    }
}

bool Group::Wait()
{
    if (m_live > 0)
    {
        // Not kill-aware: the join must complete even if the owner is being torn down,
        // mirroring a destructor's obligation to reap its children. The children
        // themselves are kill-aware and will unwind; Wait returns once they have.
        //
        CoordinateWith(m_owner, &m_done);
    }
    return !m_failed;
}

} // end namespace coop
