#include "coordinator_extension.h"

#include "context.h"
#include "coordinator.h"

namespace coop
{

void CoordinatorExtension::Block(Context* c)
{
    c->Block();
}

void CoordinatorExtension::AddAsBlocked(Coordinator* c, Coordinate* ord)
{
    c->AddAsBlocked(ord);
}

void CoordinatorExtension::RemoveAsBlocked(Coordinator* c, Coordinate* ord)
{
    c->RemoveAsBlocked(ord);
}

bool CoordinatorExtension::HeldBy(Coordinator* c, Context* ctx)
{
    return c->HeldBy(ctx);
}

void CoordinatorExtension::Shutdown(Coordinator* c, Context* ctx)
{
    c->Shutdown(ctx);
}

} // end namespace coop
