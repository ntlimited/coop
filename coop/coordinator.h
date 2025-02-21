#pragma once

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

struct Context;

// The coordinator primitive maintains a waiter list of contexts that blocked on acquiring the
// coordinator. All APIs must be called with the current context.
//
struct Coordinator
{
    Coordinator(const Coordinator&) = delete;
    Coordinator(Coordinator&&) = delete;

    Coordinator();

    bool TryAcquire(Context*);

    void Acquire(Context*);

    // Release the coordinator, unblocking the context (if one exists) at the head of the wait
    // list. By default (schedule = true), the unblocked context will immediately be switched to
    // and execute; alternatively, Release can be made to return immediately and simply switch
    // the unblocked context to yield.
    //
    void Release(Context*, const bool schedule = true);

  private:
    Context* m_heldBy;

    Context* m_head;
    Context* m_tail;
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
