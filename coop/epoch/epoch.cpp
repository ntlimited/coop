#include "epoch.h"
#include "coop/cooperator.h"
#include "coop/perf/probe.h"

namespace coop
{
namespace epoch
{

static __thread Manager* t_manager{nullptr};

Manager* GetManager()
{
    return t_manager;
}

void SetManager(Manager* mgr)
{
    t_manager = mgr;
}

// ---- Manager ----

Manager::Manager()
: m_cooperator(Cooperator::thread_cooperator)
{
    assert(m_cooperator != nullptr &&
        "epoch::Manager created outside a cooperator thread; call from a bootstrap task");
}

Manager::Manager(Cooperator* co)
: m_cooperator(co)
{
    assert(co != nullptr);
}

Manager::~Manager()
{
    // Reset the watermark so other cooperators' SafeEpoch() doesn't block on a dead manager.
    //
    m_cooperator->m_epochWatermark.store(Epoch::Alive(), std::memory_order_release);
}

void Manager::PublishWatermark()
{
    // Scan all contexts on the local cooperator (same-thread, no atomic reads needed for State).
    // Store the result atomically so remote cooperators can read it via SafeEpoch().
    //
    Epoch safe = Epoch::Alive();

    m_cooperator->VisitContexts([&](Context* ctx) -> bool
    {
        epoch::State& state = ctx->m_epochState;
        if (!state.traversal.IsUnpinned() && state.traversal < safe)
        {
            safe = state.traversal;
        }
        if (!state.application.IsUnpinned() && state.application < safe)
        {
            safe = state.application;
        }
        return true;
    });

    m_cooperator->m_epochWatermark.store(safe, std::memory_order_release);
}

Epoch Manager::Advance()
{
    m_current = m_current.Next();
    COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(),
        perf::Counter::EpochAdvance);
    return m_current;
}

Epoch Manager::Enter()
{
    assert(t_manager == this && "epoch::Manager not set for this cooperator; was bootstrap skipped?");
    return Enter(Self());
}

Epoch Manager::Enter(Context* ctx)
{
    ctx->m_epochState.traversal = m_current;
    PublishWatermark();
    return m_current;
}

void Manager::Exit()
{
    assert(t_manager == this && "epoch::Manager not set for this cooperator; was bootstrap skipped?");
    Exit(Self());
}

void Manager::Exit(Context* ctx)
{
    ctx->m_epochState.traversal = Epoch::Unpinned();
    PublishWatermark();
}

void Manager::Pin(Epoch epoch)
{
    assert(t_manager == this && "epoch::Manager not set for this cooperator; was bootstrap skipped?");
    Pin(Self(), epoch);
}

void Manager::Pin(Context* ctx, Epoch epoch)
{
    assert(!epoch.IsUnpinned() && "pin epoch must be non-zero");
    assert(epoch <= m_current && "cannot pin at a future epoch");
    ctx->m_epochState.application = epoch;
    PublishWatermark();
    COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(),
        perf::Counter::EpochPin);
}

void Manager::Unpin()
{
    assert(t_manager == this && "epoch::Manager not set for this cooperator; was bootstrap skipped?");
    Unpin(Self());
}

void Manager::Unpin(Context* ctx)
{
    ctx->m_epochState.application = Epoch::Unpinned();
    PublishWatermark();
    COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(),
        perf::Counter::EpochUnpin);
}

void Manager::Retire(RetireEntry* entry)
{
    assert(t_manager == this && "epoch::Manager not set for this cooperator; was bootstrap skipped?");
    entry->m_retiredAt = m_current;
    entry->m_next = nullptr;

    if (m_retireTail)
    {
        m_retireTail->m_next = entry;
    }
    else
    {
        m_retireHead = entry;
    }
    m_retireTail = entry;
    m_retireCount++;
}

Manager::PinSnapshot Manager::SnapshotPins()
{
    PinSnapshot snap{};
    snap.oldestTraversal = Epoch::Alive();
    snap.oldestApplication = Epoch::Alive();

    m_cooperator->VisitContexts([&](Context* ctx) -> bool
    {
        epoch::State& state = ctx->m_epochState;
        if (!state.traversal.IsUnpinned())
        {
            snap.traversalPins++;
            if (state.traversal < snap.oldestTraversal)
                snap.oldestTraversal = state.traversal;
        }
        if (!state.application.IsUnpinned())
        {
            snap.applicationPins++;
            if (state.application < snap.oldestApplication)
                snap.oldestApplication = state.application;
        }
        return true;
    });

    return snap;
}

Epoch Manager::SafeEpoch()
{
    // Read the published watermark from every cooperator in the registry. Each watermark is
    // the minimum pinned epoch across all contexts on that cooperator, or Alive() if none
    // are pinned. We take the global minimum — reclamation is blocked by the oldest reader
    // anywhere in the system.
    //
    Epoch safe = Epoch::Alive();

    Cooperator::VisitRegistry([&](Cooperator* co) -> bool
    {
        Epoch w = co->m_epochWatermark.load(std::memory_order_acquire);
        if (w < safe)
        {
            safe = w;
        }
        return true;
    });

    return safe;
}

size_t Manager::Reclaim()
{
    Epoch safe = SafeEpoch();
    size_t reclaimed = 0;

    while (m_retireHead && m_retireHead->m_retiredAt < safe)
    {
        auto* entry = m_retireHead;
        m_retireHead = entry->m_next;
        if (!m_retireHead)
        {
            m_retireTail = nullptr;
        }
        m_retireCount--;
        entry->reclaim(entry);
        reclaimed++;
    }

    return reclaimed;
}

} // end namespace coop::epoch
} // end namespace coop
