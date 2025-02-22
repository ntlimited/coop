#pragma once

namespace coop
{

struct Coordinator;
struct Context;

// CoordinatorExtension, per Coordinator docs, prevents casual misuse of Coordinator internals
//
struct CoordinatorExtension
{
  protected:
    void AddAsBlocked(Coordinator* c, Context* ctx)
    {
        c->AddAsBlocked(ctx);
    }

    bool RemoveAsBlocked(Coordinator* c, Context* ctx)
    {
        return c->RemoveAsBlocked(ctx);
    }

    bool HeldBy(Coordinator* c, Context* ctx)
    {
        return c->HeldBy(ctx);
    }
};

} // end namespace coop
