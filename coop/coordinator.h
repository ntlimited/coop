#pragma once

#include "detail/embedded_list.h"

namespace coop
{

struct Coordinator;
struct Context;
struct CoordinatorExtension;
struct Signal;

// Coordinated is the link between a blocked Context and the Coordinator it is waiting on. Each
// instance sits on a Coordinator's wait list and tracks whether the coordination was satisfied
// (i.e. the context was granted ownership).
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

    // Destroying a Coordinated while it is still on a Coordinator's wait list would corrupt
    // the list.
    //
    ~Coordinated()
    {
        assert(Disconnected());
    }

    bool Satisfied() const
    {
        return m_satisfied;
    }

  private:
    friend struct Coordinator;
    friend struct CoordinatorExtension;
    friend struct Signal;

    void Satisfy()
    {
        m_satisfied = true;
    }

    void Reset()
    {
        m_satisfied = false;
    }

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
    : m_heldBy(ctx)
    {
    }

    bool IsHeld() const;

    // TryAcquire, Acquire, and Release all act in the manner that one would expect from a mutex
    // analogue. For multi-coordinator and timeout behaviors, see the CoordinateWith functionality
    // in coordinate_with.h.
    //
    bool TryAcquire(Context*);

    void Acquire(Context*);

    // Barrier: blocks until the current holder releases, then immediately passes through. No-op
    // if the coordinator is not held.
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

  private:
    friend struct CoordinatorExtension;
    friend struct Signal;

    void AddAsBlocked(Coordinated*);
    void RemoveAsBlocked(Coordinated*);
    bool HeldBy(Context* ctx);

  private:
    Context* m_heldBy;
    Coordinated::List m_blocking;
};

} // end namespace coop
