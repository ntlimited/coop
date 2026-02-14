#pragma once

namespace coop
{

struct Coordinated;
struct Coordinator;
struct Context;

// CoordinatorExtension, per Coordinator docs, prevents casual misuse of Coordinator internals
//
struct CoordinatorExtension
{
    void Block(Context* c);

    void AddAsBlocked(Coordinator* c, Coordinated* ord);

    void RemoveAsBlocked(Coordinator* c, Coordinated* ord);

    bool HeldBy(Coordinator* c, Context* ctx);

    void SetContext(Coordinated* c, Context* ctx);
};

} // end namespace coop
