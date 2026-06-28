#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/self.h"
#include "coop/time/interval.h"
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

    // Size for n cooperators. The remaining parameters tune the idle stealer's park/re-check
    // policy, which trades idle-core wakeup rate against worst-case rebalancing latency.
    //
    // An idle stealer parks on an io_uring timer, wakes, re-checks for stealable work, and parks
    // again. A short interval notices clustered work quickly (tight rebalancing tail) but a fully
    // idle core then wakes tens of thousands of times a second doing nothing -- a steady timer-CQE
    // drip that scales with the number of idle cores. To get both, the interval is adaptive: it
    // starts at recheckMin and, after idleGrowAfter consecutive empty re-checks, doubles each empty
    // re-check up to recheckMax. Any successful pull (local work or a steal) snaps it back to
    // recheckMin, so a stealer that is finding work stays aggressive and only a persistently
    // quiescent core coasts up to the cap. recheckMin therefore still bounds rebalancing latency
    // while load is present; recheckMax bounds it after the core has gone fully idle -- the worst
    // case a cross-thread steal-wake would address.
    //
    void Init(int n,
              time::Interval recheckMin    = std::chrono::microseconds(10),
              time::Interval recheckMax    = std::chrono::microseconds(200),
              int            idleGrowAfter = 8);

    // Opt the cooperator into this grid: assign it the next shard, set its participation field, and
    // spawn its stealer. Call once per cooperator, after Init, before sheddding to it.
    //
    void Join(Cooperator* co);

    // Owner of shard's cooperator only (the local stealer, or in-cooperator code that ran there).
    //
    void ShedErg(int shard, Erg* e) { m_shards.Shed(shard, e); }

    // Stealer instrumentation, summed across all shards. Written only by the idle daemon stealers
    // (off the in-cooperator Shed hot path); meant to be read after quiescence. Parks counts idle
    // timer arms -- one per idle-core wakeup, the cost the backoff exists to cut. Pulls counts work
    // units a stealer ran. MaxIdleRun is the longest observed run of consecutive empty re-checks.
    // A production build would route these through the perf system rather than carry the atomics.
    //
    uint64_t Parks() const { return m_parks.load(std::memory_order_relaxed); }
    uint64_t Pulls() const { return m_pulls.load(std::memory_order_relaxed); }
    uint64_t MaxIdleRun() const { return m_maxIdleRun.load(std::memory_order_relaxed); }

  private:
    void StealerLoop(Context* ctx, int shard);

    detail::Shards<Erg*>             m_shards;
    std::unique_ptr<Participation[]> m_parts;
    std::atomic<int>                 m_joined{0};
    int                              m_n = 0;
    time::Interval                   m_recheckMin    = std::chrono::microseconds(10);
    time::Interval                   m_recheckMax    = std::chrono::microseconds(200);
    int                              m_idleGrowAfter = 8;

    std::atomic<uint64_t>            m_parks{0};
    std::atomic<uint64_t>            m_pulls{0};
    std::atomic<uint64_t>            m_maxIdleRun{0};
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
