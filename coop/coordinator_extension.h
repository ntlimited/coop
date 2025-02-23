#pragma once

namespace coop
{

struct Coordinate;
struct Coordinator;
struct Context;

// CoordinatorExtension, per Coordinator docs, prevents casual misuse of Coordinator internals
//
struct CoordinatorExtension
{
    void Block(Context* c);

    void AddAsBlocked(Coordinator* c, Coordinate* ord);

    void RemoveAsBlocked(Coordinator* c, Coordinate* ord);

    bool HeldBy(Coordinator* c, Context* ctx);

    void Shutdown(Coordinator* c, Context* ctx);
};

} // end namespace coop
