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
// stealer (hand off async follow-on via Continuations). v1 uses new/delete (malloc's cross-thread
// free is correct); a per-cooperator slab with cross-thread free is a later optimization.
//
struct Erg : Thunk
{
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
    e->Run();
    delete e;
}

} // end namespace work
} // end namespace coop
