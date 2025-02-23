#include "handle.h"

#include "cooperator.h"
#include "context.h"
#include "self.h"

namespace coop
{

void Handle::Kill()
{
    m_context->GetCooperator()->BoundarySafeKill(this, false);
}

} // end namespace coop
