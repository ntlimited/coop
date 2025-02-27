#include "router.h"

#include "handle.h"

namespace coop
{

namespace network
{

// Silly helper method to launder `Handle::GetCoordinator()`. Probably gets 100% inlined but also
// could probably get rid of this hop without real issue too.
//
Coordinator* Router::GetCoordinator(Handle* h) const
{
    return h->GetCoordinator();
}

void Router::SetRouter(Handle* h)
{
    h->SetRouter(this);
}

} // end namespace coop::network
} // end namespace coop
