#pragma once

#include <stdint.h>

namespace coop
{

// A Coordinator is the cooperator version of lock semantics, allowing tasks to wait for each
// other natively. The concept is simple: we can just check if the coordinator is held, and if
// so, yield ourselves into blocked state and add ourselves to the coordinator's blocked list.
// We can only be blocked on one thing at once, so the execution context is able to hold this
// part of the state itself to form a linked list.
//
// This gets somewhat muddy with certain patterns: e.g., a semaphore where anyone can unlock the
// underlying mutex regardless of who locked it when the last resources was taken. For this
// complexity, the natural extension of storing "who is holding this lock" is not meaningfully
// and/or completely implemented.
//

// In general, base Coordinator instances should be passed in to higher level coordination
// mechanisms vs embedded within them, which allows for simpler composition.
//

struct Context;

// CoordinatorExtension is a type that has access to certain Coordiantor internals which are
// used for composing higher-level functionality on top of the mechanics that Coordinators
// implement for the cooperative multitasking system regarding blocking state transitions.
//
struct CoordinatorExtension;


// CoordinatorPatterns allow the Coordinator type to have different contracts under the hood
// for different purposes.
//
// In theory, this could be replaced by a class heirarchy but I didn't want to introduce that
// at this point. This became necessary in the first place in order to allow the equivalent of
//

// The coordinator primitive maintains a waiter list of contexts that blocked on acquiring the
// Coordinator. Acquire methods must be called with the current context but release doesn't
// currently matter. This is something I should make an actual call on.
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
    void AddAsBlocked(Context* ctx);
    bool RemoveAsBlocked(Context* ctx); 
    bool HeldBy(Context* ctx);
    void Shutdown();

  private:
    Context* m_heldBy;

    Context* m_head;
    Context* m_tail;

    bool m_shutdown;
};

struct CoordinatedSemaphore
{
    CoordinatedSemaphore(int initial = 0)
    : m_avail(initial)
    {
    }

    bool TryAcquire(Context*);

    void Acquire(Context*);
    void Release(Context*);

  private:
    int m_avail;
    Coordinator m_coordinator;
};

} // end namespace coop
