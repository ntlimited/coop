#pragma once

#include "coop/coordinator.h"
#include "coop/context.h"

namespace coop
{

// CoordinatorExtension, per Coordinator docs, prevents casual misuse of Coordinator internals
//
struct CoordinatorExtension
{
    void Block(Context* c)
    {
        c->Block();
    }

    void AddAsBlocked(Coordinator* c, Coordinated* ord)
    {
        c->AddAsBlocked(ord);
    }

    void RemoveAsBlocked(Coordinator* c, Coordinated* ord)
    {
        c->RemoveAsBlocked(ord);
    }

    bool HeldBy(Coordinator* c, Context* ctx)
    {
        return c->HeldBy(ctx);
    }

    void SetContext(Coordinated* c, Context* ctx)
    {
        c->SetContext(ctx);
    }
};

} // end namespace coop
