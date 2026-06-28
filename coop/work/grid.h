#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/self.h"
#include "detail/shards.h"
#include "erg.h"

namespace coop
{
namespace work
{

class Grid;

// What a cooperator's Cooperator::m_participation points to once it has joined a Grid: the grid and
// this cooperator's shard index. Owned by the Grid (one per joined cooperator).
//
struct Participation
{
    Grid* grid  = nullptr;
    int   shard = -1;
};

// A Grid is the work-sharing domain: a set of cooperators that balance Ergs among themselves by
// stealing. Each joined cooperator owns one shard (a Deque) and runs a daemon stealer context that
// pulls local work, steals from peers, runs the Erg to completion, and parks on a short io_uring
// timer when idle (re-checking for stealable work). Opt-in: a cooperator that never joins a Grid is
// untouched, and Shed() on it falls back to Spawn.
//
class Grid
{
  public:
    Grid() = default;
    Grid(const Grid&) = delete;
    Grid& operator=(const Grid&) = delete;

    // Size for n cooperators; recheckUs is the idle stealer's park/re-check interval. The interval
    // bounds how quickly an idle stealer notices stealable work, so it caps the worst-case
    // rebalancing latency; ~10us keeps clustered makespan stable. (A cross-thread steal-wake would
    // let this be larger -- a later refinement.)
    //
    void Init(int n, uint32_t recheckUs = 10);

    // Opt the cooperator into this grid: assign it the next shard, set its participation field, and
    // spawn its stealer. Call once per cooperator, after Init, before sheddding to it.
    //
    void Join(Cooperator* co);

    // Owner of shard's cooperator only (the local stealer, or in-cooperator code that ran there).
    //
    void ShedErg(int shard, Erg* e) { m_shards.Shed(shard, e); }

    uint32_t Recheck() const { return m_recheckUs; }

  private:
    void StealerLoop(Context* ctx, int shard);

    detail::Shards<Erg*>             m_shards;
    std::unique_ptr<Participation[]> m_parts;
    std::atomic<int>                 m_joined{0};
    int                              m_n = 0;
    uint32_t                         m_recheckUs = 10;
};

} // end namespace work

// Shed -- the balanced-work verb, sibling to Spawn. On a cooperator that has joined a work::Grid it
// sheds a run-to-completion Erg that any participating cooperator may steal; otherwise it falls back
// to Spawn (the "shed = spawn" default). The run-to-completion contract (no suspending) applies only
// on the Grid path, where the Erg runs on a shared stealer; the Spawn fallback gives fn its own
// context and may block.
//
template<typename Fn>
inline void Shed(Fn&& fn)
{
    Cooperator* co = GetCooperator();
    if (work::Participation* p = co->m_participation)
    {
        p->grid->ShedErg(p->shard, work::MakeErg(std::forward<Fn>(fn)));
    }
    else
    {
        // shed = spawn: fn is nullary (a morsel; uses Self() for context access), so adapt it to
        // Spawn's (Context*) signature.
        //
        Spawn([f = std::forward<Fn>(fn)](Context*) mutable { f(); });
    }
}

} // end namespace coop
