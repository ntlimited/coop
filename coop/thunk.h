#pragma once

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

} // end namespace coop
