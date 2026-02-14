#include "context.h"

#include "cooperator.h"
#include "self.h"

namespace coop
{

Signal* Context::Handle::GetKilledSignal()
{
    // TODO add some "you're in the right cooperator" asserts here and everywhere
    //
    return m_context->GetKilledSignal();
}

// Note that the kill works on the handle, not the context
//
void Context::Handle::Kill()
{
    m_context->GetCooperator()->BoundarySafeKill(this, false);
}

} // end namespace coop
