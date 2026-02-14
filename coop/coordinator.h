#pragma once

#include <stdint.h>

#include "embedded_list.h"

namespace coop
{

struct Coordinator;
struct Context;
struct CoordinatorExtension;
struct Signal;

// Coordinated is an internal type used to maintain the necessary metadata for coordinated
// operations. It is also usable with the CoordinatorExtension type that launders access to
// its internal APIs in order to create higher-level coordination constructs.
//
// TODO any value to including the Coordinator in the struct? Probably not and would likely
// lead to bad patterns.
//
struct Coordinated : EmbeddedListHookups<Coordinated>
{
    using List = EmbeddedList<Coordinated>;

    Coordinated(Context* ctx)
    : m_context(ctx)
    , m_satisfied(false)
    {
    }

    Coordinated()
    : m_context(nullptr)
    , m_satisfied(false)
    {
    }

    // Fun classes of bugs around this
    //
    ~Coordinated()
    {
        assert(Disconnected());
    }

    bool Satisfied() const
    {
        return m_satisfied;
    }

    // The satisfied flag is set when the Coordinated instance is pulled off the blocked list and
    // its context is passed control. This is almost isomorphic to the 'heldBy' tracking in the
    // Coordinator instance itself, but there is a nuance with "reentrancy" in the case where we
    // compose the Coordinate(...) functionality and we need to distinguish the fact that an
    // instance was already held by the same context (and it is waiting on another to unlock it
    // out of band, e.g. a kill signal...)
    //
    operator bool() const
    {
        return m_satisfied;
    }

  private:
    friend struct Coordinator;
    friend struct CoordinatorExtension;
    friend struct Signal;

    void SetContext(Context* ctx)
    {
        m_context = ctx;
    }

    Context* GetContext()
    {
        return m_context;
    }

    Context*    m_context;
    bool        m_satisfied;
};

// Coordinator is the sole coordination primitive for contexts. It is heavily used and non-virtual,
// which also means that higher level constructs are not themselves Coordinators.
//
// It is generally better form to take a pointer to a Coordinator as an argument to build higher
// level items, versus embed one's own Coordinator.
//
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
        m_heldBy = ctx;
    }

    bool IsHeld() const;

    // TryAcquire, Acquire, and Release all act in the manner that one would expect from a mutex
    // analogue. For timeout based behaviors, see the coop::time package and the multi coordinator
    // aka 'CoordinateWith' functionality.
    //
    bool TryAcquire(Context*);

    void Acquire(Context*);

    // Dumb helper for acquire-and-release
    //
    void Flash(Context*);

    // Release the coordinator, unblocking the context (if one exists) at the head of the wait
    // list. By default (schedule = true), the unblocked context will immediately be switched to
    // and execute; alternatively, Release can be made to return immediately and simply switch
    // the unblocked context to yield.
    //
    void Release(Context*, const bool schedule = true);

    // The lack of move and copy semantics make this true
    //
    bool operator==(Coordinator* other) const
    {
        return this == other;
    }

  protected:
    friend struct CoordinatorExtension;
    friend struct Signal;

    void AddAsBlocked(Coordinated*);
    void RemoveAsBlocked(Coordinated*);
    bool HeldBy(Context* ctx);

//  private:
    Context* m_heldBy;
    Coordinated::List m_blocking;
};

} // end namespace coop
