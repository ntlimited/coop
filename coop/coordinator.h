#pragma once

#include <stdint.h>

#include "embedded_list.h"

namespace coop
{

struct Coordinator;
struct Context;
struct CoordinatorExtension;

// Coordinate is an internal type used to maintain the necessary metadata for coordinated
// operations. It is also usable with the CoordinatorExtension type that launders access to
// its internal APIs in order to create higher-level coordination constructs.
//
// TODO any value to including the Coordinator in the struct? Probably not and would likely
// lead to bad patterns.
//
struct Coordinate : EmbeddedListHookups<Coordinate>
{
    using List = EmbeddedList<Coordinate>;

    Coordinate(Context* ctx)
    : m_context(ctx)
    {
    }

  private:
    friend struct Coordinator;
    friend struct CoordinatorExtension;

    Context* GetContext()
    {
        return m_context;
    }

    Context*    m_context;
};

struct Coordinator
{
    Coordinator(const Coordinator&) = delete;
    Coordinator(Coordinator&&) = delete;
    
    // Start out unheld
    //
    Coordinator();

    // Start out held
    //
    Coordinator(Context* ctx)
    : Coordinator()
    {
        Acquire(ctx);
    }

    bool IsHeld() const;

    bool TryAcquire(Context*);

    void Acquire(Context*);

    // Release the coordinator, unblocking the context (if one exists) at the head of the wait
    // list. By default (schedule = true), the unblocked context will immediately be switched to
    // and execute; alternatively, Release can be made to return immediately and simply switch
    // the unblocked context to yield.
    //
    void Release(Context*, const bool schedule = true);

  protected:
    friend struct CoordinatorExtension;
    void AddAsBlocked(Coordinate*);
    void RemoveAsBlocked(Coordinate*); 
    bool HeldBy(Context* ctx);
    void Shutdown(Context* ctx);

  private:
    Context* m_heldBy;
    Coordinate::List m_blocking;

    bool m_shutdown;
};

} // end namespace coop
