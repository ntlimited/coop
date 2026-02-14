#include "coordinator_extension.h"

#include "context.h"
#include "coordinator.h"

namespace coop
{

void CoordinatorExtension::Block(Context* c)
{
    c->Block();
}

void CoordinatorExtension::AddAsBlocked(Coordinator* c, Coordinated* ord)
{
    c->AddAsBlocked(ord);
}

void CoordinatorExtension::RemoveAsBlocked(Coordinator* c, Coordinated* ord)
{
    c->RemoveAsBlocked(ord);
}

bool CoordinatorExtension::HeldBy(Coordinator* c, Context* ctx)
{
    return c->HeldBy(ctx);
}

void CoordinatorExtension::SetContext(Coordinated* c, Context* ctx)
{
    c->SetContext(ctx);
}

} // end namespace coop
