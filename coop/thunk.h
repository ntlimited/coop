#pragma once

#include <cassert>

namespace coop
{

// A Thunk is a stackless, run-to-completion unit of work: a type-erased nullary action that runs
// start-to-finish on whatever context invokes it and never suspends mid-execution. It is the shared
// shape of two scheduling species that differ in disposition, not in kind:
//
//   - Continuation: in-cooperator, fired by a Coordinator release. Never migrates, so no atomics.
//   - Erg:          cross-core, shed into a work::Grid and run by a stealer on whatever cooperator
//                   pulls it.
//
// What distinguishes them is how they are scheduled and whether they cross cores -- both are Thunks.
// (An Erg, once it lands and runs, typically decomposes its async follow-on into Continuations.)
//
struct Thunk
{
    virtual void Run() = 0;
    virtual ~Thunk() = default;
};

namespace detail
{

// Debug-only "running inside a Thunk" tracking. While a Thunk executes it borrows a host context
// (the cooperator's green thread for a Continuation drain, or a stealer for an Erg), so suspending
// operations -- Yield, Block, and the blocking IO/Coordinate paths that funnel through them -- would
// stall the host rather than the (stackless, non-suspending) Thunk. The guard lets those operations
// assert at the misuse site instead of corrupting the host. Zero-cost in release: the scope and the
// assert compile away entirely.
//
#ifndef NDEBUG

// Defined in cooperator.cpp. All null-safe: with no current cooperator (e.g. a raw thread) there is
// no flag, so EnterThunk is a no-op and InThunk is false.
//
bool EnterThunk();         // set the current cooperator's flag; returns its previous value
void ExitThunk(bool prev); // restore the previous value
bool InThunk();            // current flag (false if no cooperator)

struct ThunkScope          // RAII: set on enter, restore on exit (nests correctly)
{
    bool m_prev;
    ThunkScope()  { m_prev = EnterThunk(); }
    ~ThunkScope() { ExitThunk(m_prev); }
};

inline void AssertNotInThunk()
{
    assert(!InThunk() &&
        "forbidden inside a Thunk (Continuation/Erg): run-to-completion units must not suspend");
}

#else

struct ThunkScope { ThunkScope() {} };
inline void AssertNotInThunk() {}

#endif

} // end namespace detail

} // end namespace coop
