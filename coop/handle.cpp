#include "context.h"

#include "cooperator.h"
#include "self.h"

namespace coop
{

Signal* Context::Handle::GetKilledSignal()
{
    // After the spawned context exits, ~Context clears m_context. Kill already treats
    // that as a no-op; this must too — the handle outlives the context by contract.
    //
    return m_context ? m_context->GetKilledSignal() : nullptr;
}

// Note that the kill works on the handle, not the context
//
void Context::Handle::Kill(KillCause cause /* = KillCause::Kill */)
{
    if (m_context)
    {
        m_context->GetCooperator()->BoundarySafeKill(this, false, cause);
    }
}

} // end namespace coop
