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
// The single-owner contract a reusable Erg must honor: it is never resident in a shard and running
// at the same time. The IO-pipeline pattern guarantees this -- a stage runs, submits its async op,
// and only the op's completion continuation re-sheds the Erg, by which point the prior Run() has
// fully retired. A re-shed lands on the running stealer's own local shard, so a peer cannot observe
// it until the stealer returns and loops.
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
    const bool owned = e->m_stealerOwned;
    e->Run();
    // A caller-owned (reusable) Erg may have already re-shed itself during Run(); the stealer must
    // not free it. Read m_stealerOwned BEFORE Run() so a re-shed peer touching the object cannot
    // race this load.
    //
    if (owned)
    {
        delete e;
    }
}

} // end namespace work
} // end namespace coop
