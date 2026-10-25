#pragma once

#include <type_traits>
#include <utility>

#include "coop/thunk.h"

namespace coop
{
namespace work
{

// An Erg is the cross-core Thunk species (the CGS unit of work): a stackless, run-to-completion
// morsel shed into a Grid and run by a stealer on whatever cooperator pulls it. Unlike a
// Continuation (single-cooperator, never migrates), an Erg is owned cross-thread -- run and freed on
// a possibly different core than it was shed -- so it must run to completion without blocking the
// stealer (hand off async follow-on via Continuations). The default Shed(fn) path uses new/delete
// (malloc's cross-thread free is correct); a per-cooperator slab with cross-thread free is a later
// optimization.
//
// Lifetime is a per-Erg property. A transient, allocated Erg (MakeErg) is stealer-owned: the stealer
// frees it after Run(). A reusable Erg -- a stable, long-lived work item that re-sheds itself each
// stage of an IO pipeline -- is caller-owned: the stealer runs it and does NOT free it, because the
// same object will be shed again. Caller-owned removes the per-shed malloc/free entirely for the
// re-shedding pattern (the substrate's headline workload), where the allocation is pure waste.
//
// A successful shed publishes the Erg immediately: a peer can steal it and enter the next Run()
// before the publishing Run() returns. Finish all shared-state updates before re-shedding; after
// that handoff, the old invocation may use only invocation-local state or explicitly synchronized
// external state, not the Erg's fields. A failed shed leaves ownership with the caller. Never
// publish the same Erg twice without an intervening pull.
//
// The caller must keep reusable storage alive, and m_stealerOwned unchanged, until every Run()
// invocation has retired. An IO pipeline can avoid overlapping invocations by re-shedding only
// from the submitting cooperator's completion continuation, which runs after its prior Run returns.
//
struct Erg : Thunk
{
    // false => caller-owned (reusable): RunErg runs but does not free. Default true => stealer frees
    // after Run() (the MakeErg path).
    //
    bool m_stealerOwned = true;
};

template<typename Fn>
struct ErgImpl final : Erg
{
    explicit ErgImpl(Fn fn) : m_fn(std::move(fn)) {}
    void Run() final { m_fn(); }
    Fn m_fn;
};

template<typename Fn>
inline Erg* MakeErg(Fn&& fn)
{
    return new ErgImpl<std::decay_t<Fn>>(std::forward<Fn>(fn));
}

inline void RunErg(Erg* e)
{
    coop::detail::ThunkScope inThunk;    // debug: forbid suspending inside the Erg's Run
    // Snapshot disposition before invoking user code: a reusable Erg can publish itself to a
    // peer during Run(). The retiring invocation must not access caller-owned e afterward.
    //
    const bool owned = e->m_stealerOwned;
    e->Run();
    if (owned)
    {
        delete e;
    }
}

} // end namespace work
} // end namespace coop
