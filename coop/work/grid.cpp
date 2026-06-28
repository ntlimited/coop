#include "grid.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>

#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"
#include "coop/io/handle.h"
#include "coop/io/timeout.h"

namespace coop
{
namespace work
{

void Grid::Init(int n, time::Interval recheckMin, time::Interval recheckMax, int idleGrowAfter)
{
    m_n = n;
    m_recheckMin = recheckMin;
    m_recheckMax = recheckMax;
    m_idleGrowAfter = idleGrowAfter;
    m_shards.Init(n);
    m_parts.reset(new Participation[n]);
}

void Grid::Join(Cooperator* co)
{
    const int shard = m_joined.fetch_add(1, std::memory_order_relaxed);
    assert(shard < m_n && "Grid::Join called more times than Init sized for");
    m_parts[shard].grid  = this;
    m_parts[shard].shard = shard;
    Participation* p = &m_parts[shard];

    // Run setup on the cooperator's own thread: publish its participation field, then spawn the
    // stealer as a detached daemon (so the transient setup context exiting does not kill it).
    //
    co->Submit([this, p, shard](Context* ctx)
    {
        ctx->GetCooperator()->m_participation = p;
        Spawn([this, shard](Context* s)
        {
            s->Detach();
            StealerLoop(s, shard);
        });
    });
}

void Grid::StealerLoop(Context* ctx, int shard)
{
    // Adaptive park interval: start aggressive, coast up to the cap only while persistently idle.
    // Both the interval and the idle-run counter are per-stealer locals -- no atomics, no shared
    // state, so the policy is entirely contained to this daemon (opt-in stays free).
    //
    time::Interval interval = m_recheckMin;
    int idleRun = 0;

    Coordinator& doorbell = m_parts[shard].doorbell;

    while (!ctx->IsKilled())
    {
        if (Erg* e = m_shards.Pull(shard))
        {
            RunErg(e);                                       // run-to-completion on this stealer
            m_pulls.fetch_add(1, std::memory_order_relaxed);
            interval = m_recheckMin;                         // work found: snap back to aggressive
            idleRun = 0;
            continue;
        }

        // No work to pull. Before sleeping, drain the doorbell: if a local Shed rang it while this
        // stealer was busy, TryAcquire takes ownership and we loop straight back to Pull instead of
        // arming a timer. This is the self-heal for sheds that land while the stealer is not parked
        // -- one extra non-sleeping re-check, never a spin (the doorbell is single-bit).
        //
        if (doorbell.TryAcquire(ctx))
        {
            continue;
        }

        // Idle: park until either the recheck timer fires or a local Shed rings the doorbell,
        // whichever comes first (kill-aware). The timer bounds cross-cooperator rebalancing latency
        // and is what lets this core steal from busy peers; the doorbell bounds local-shed pickup to
        // a single scheduler pass regardless of how far the timer has backed off. Each consecutive
        // empty re-check past idleGrowAfter doubles the interval up to recheckMax, so a fully
        // quiescent core stops waking tens of thousands of times a second once it is clear there is
        // nothing to steal -- and the doorbell means that backoff no longer delays its own work.
        //
        if (++idleRun > m_idleGrowAfter && interval < m_recheckMax)
        {
            interval = std::min(interval * 2, m_recheckMax);
        }
        m_parks.fetch_add(1, std::memory_order_relaxed);
        for (uint64_t prev = m_maxIdleRun.load(std::memory_order_relaxed);
             (uint64_t)idleRun > prev &&
             !m_maxIdleRun.compare_exchange_weak(prev, (uint64_t)idleRun,
                                                 std::memory_order_relaxed);)
        {
        }

        Coordinator coord;
        io::Handle handle(ctx, GetUring(), &coord);
        io::Timeout(handle, interval);

        // Block on the doorbell and the timer together. On a doorbell win the timer is still in
        // flight; the Handle destructor cancels and drains it. A doorbell win re-takes ownership of
        // the doorbell, so the next idle pass parks correctly rather than spinning.
        //
        CoordinateWithKill(ctx, &doorbell, &coord);
    }
}

} // end namespace work
} // end namespace coop
