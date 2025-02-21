#include "handle.h"

#include "context.h"

namespace coop
{

void Handle::Kill()
{
    m_context->Kill();
}

} // end namespace coop
