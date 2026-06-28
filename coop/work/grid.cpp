#include "grid.h"

#include <cassert>
#include <chrono>

#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/io/handle.h"
#include "coop/io/timeout.h"

namespace coop
{
namespace work
{

void Grid::Init(int n, time::Interval recheck)
{
    m_n = n;
    m_recheck = recheck;
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
    while (!ctx->IsKilled())
    {
        if (Erg* e = m_shards.Pull(shard))
        {
            RunErg(e);                                       // run-to-completion on this stealer
            continue;
        }

        // Idle: park on a short io_uring timer (kill-aware), then re-check and steal. Parking in
        // io_uring yields the core; the recheck interval bounds cross-cooperator rebalancing
        // latency. A local shed is picked up on the next recheck (early-wake is a later refinement).
        //
        Coordinator coord;
        io::Handle handle(ctx, GetUring(), &coord);
        io::Timeout(handle, m_recheck);
        handle.WaitKill();
    }
}

} // end namespace work
} // end namespace coop
