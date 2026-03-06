#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

#include "coop/context_var.h"

namespace coop
{

struct Context;
struct Cooperator;

namespace epoch
{

// Epoch: strongly-typed monotonic counter value. Wraps uint64_t to prevent accidental
// mixing with unrelated integers and to give sentinels readable names.
//
// Sentinels:
//   Epoch::Unpinned() — zero, used in State slots to mean "not currently pinned"
//   Epoch::Alive()    — UINT64_MAX, used in Node::m_deathTxn to mean "not yet deleted"
//
struct Epoch
{
    static constexpr Epoch Unpinned() { return Epoch{0}; }
    static constexpr Epoch Alive()    { return Epoch{UINT64_MAX}; }

    Epoch() = default;
    constexpr explicit Epoch(uint64_t v) : m_value(v) {}

    bool IsUnpinned() const { return m_value == 0; }
    bool IsAlive()    const { return m_value == UINT64_MAX; }

    Epoch Next() const { return Epoch{m_value + 1}; }

    auto operator<=>(Epoch const&) const = default;

private:
    friend struct Manager;
    friend struct RetireEntry;
    uint64_t m_value{0};
};

// Per-context epoch participation. Two pin slots with clear ownership:
//
//   traversal:   held during data structure access. Short-lived, managed by coop internals
//                (Guard). Pinned on entry to a traversal, unpinned on exit. Prevents
//                reclamation of nodes that an active traversal might be walking through.
//
//   application: held for transaction lifetime or similar app-layer concerns. Long-lived,
//                managed entirely by the application. Coop never touches this slot. Prevents
//                reclamation of versions that a transaction's snapshot can still see.
//
// Epoch::Unpinned() means "not pinned." SafeEpoch scans both slots across all contexts;
// reclamation is blocked by the minimum non-Unpinned pin regardless of which slot holds it.
//
struct State
{
    Epoch traversal{Epoch::Unpinned()};
    Epoch application{Epoch::Unpinned()};
};

// Type-erased retire entry. Embed this in your own type for zero-allocation retirement.
//
// The reclaim callback receives the RetireEntry pointer; the implementation static_casts back
// to the concrete type and frees it however is appropriate (slab return, delete, etc.).
//
//   struct MyNode : epoch::RetireEntry {
//       Key key;
//       Value value;
//   };
//
//   node->reclaim = [](RetireEntry* e) { allocator.Free(static_cast<MyNode*>(e)); };
//   manager.Retire(node);
//
// Singly-linked: retire lists are strict FIFOs (append at tail, consume from head, never
// remove from middle). No need for doubly-linked overhead.
//
struct RetireEntry
{
    RetireEntry*    m_next{nullptr};
    Epoch           m_retiredAt{Epoch::Unpinned()};
    void            (*reclaim)(RetireEntry*);
};

// Per-cooperator epoch manager. One instance per cooperator thread, accessed via __thread
// pointer set during a bootstrap task.
//
// The epoch is a single monotonically increasing counter. All operations are cooperator-local
// (no cross-thread synchronization). Cross-cooperator aggregation, if ever needed, is done
// externally via the cooperator registry with appropriate locking.
//
// The manager does not spawn its own GC task — that is a policy decision for the consumer.
// It provides the mechanism: Retire to enqueue, SafeEpoch to compute the reclamation horizon,
// Reclaim to free everything below it.
//
struct Manager
{
    // Default constructor: reads Cooperator::thread_cooperator. Must be called from a
    // cooperator thread (i.e. inside a running cooperator). Asserts on misuse.
    //
    Manager();

    // Cooperator-owned constructor: takes an explicit back-pointer. Used when Manager is
    // a member of Cooperator itself — thread_cooperator is not yet set at that point.
    //
    explicit Manager(Cooperator* co);

    ~Manager();

    // ---- Epoch counter ----

    // Current global epoch for this cooperator.
    //
    Epoch Current() const { return m_current; }

    // Advance the global epoch. Returns the new value. Typically called once per scheduler
    // loop iteration or once per transaction commit — the right cadence depends on the consumer.
    //
    Epoch Advance();

    // ---- Traversal pins (coop-managed, short-lived) ----

    // Pin the calling context at the current epoch. Returns the pinned epoch value.
    // Use Guard for RAII.
    //
    Epoch Enter();
    Epoch Enter(Context* ctx);

    // Unpin the calling context's traversal slot.
    //
    void Exit();
    void Exit(Context* ctx);

    // ---- Application pins (app-managed, long-lived) ----

    // Pin the calling context's application slot at the given epoch. The epoch value is
    // supplied by the caller (typically the current epoch at transaction begin).
    //
    void Pin(Epoch epoch);
    void Pin(Context* ctx, Epoch epoch);

    // Unpin the calling context's application slot.
    //
    void Unpin();
    void Unpin(Context* ctx);

    // ---- Retirement ----

    // Retire an entry for later reclamation. Sets retiredAt to the current epoch and
    // appends to the retire queue. The entry's reclaim callback will be invoked by
    // Reclaim() once the safe epoch advances past retiredAt.
    //
    void Retire(RetireEntry* entry);

    // ---- Reclamation ----

    // Compute the minimum epoch across all pinned contexts on this cooperator. Returns
    // Epoch::Alive() if no context is pinned (everything is reclaimable).
    //
    Epoch SafeEpoch();

    // Reclaim all retired entries with retiredAt < SafeEpoch(). Returns the number
    // of entries reclaimed. This is O(reclaimed) — the retire list is ordered by epoch,
    // so we stop at the first entry that's still protected.
    //
    size_t Reclaim();

    // Number of entries awaiting reclamation.
    //
    size_t PendingCount() const { return m_retireCount; }

private:
    // Recompute the minimum pinned epoch across all contexts on m_cooperator and publish
    // it to m_cooperator->m_epochWatermark. Called after every pin/unpin so that remote
    // cooperators' SafeEpoch() always sees an up-to-date value.
    //
    void PublishWatermark();

    Epoch m_current{Epoch{1}};  // starts at 1; Epoch::Unpinned() (0) means "not pinned"

    // Back-pointer to the owning cooperator. Set in the Manager constructor from
    // Cooperator::thread_cooperator. Used to publish the watermark and to scan contexts.
    //
    Cooperator* m_cooperator;

    // FIFO retire queue: append at tail, consume from head. Entries are naturally
    // ordered by epoch since retirement always stamps the current epoch.
    //
    RetireEntry*    m_retireHead{nullptr};
    RetireEntry*    m_retireTail{nullptr};
    size_t          m_retireCount{0};
};

// RAII traversal guard. Pins the current context's traversal slot on construction,
// unpins on destruction.
//
//   {
//       epoch::Guard guard(manager);
//       // safe to traverse epoch-protected data structures
//   }
//   // traversal unpinned — reclamation can proceed
//
struct Guard
{
    Guard(Manager& mgr)
    : m_mgr(mgr)
    , m_ctx(Self())
    , m_epoch(mgr.Enter(m_ctx))
    {
    }

    Guard(Manager& mgr, Context* ctx)
    : m_mgr(mgr)
    , m_ctx(ctx)
    , m_epoch(mgr.Enter(ctx))
    {
    }

    ~Guard()
    {
        m_mgr.Exit(m_ctx);
    }

    Guard(Guard const&) = delete;
    Guard(Guard&&) = delete;

    Epoch PinnedEpoch() const { return m_epoch; }

private:
    Manager&    m_mgr;
    Context*    m_ctx;
    Epoch       m_epoch;
};

// ---- Thread-local manager access ----

// Set by bootstrap task during cooperator startup. All epoch operations on a cooperator
// thread go through this pointer.
//
Manager* GetManager();
void SetManager(Manager* mgr);

// Access the per-context epoch state. Public for debugging and cross-cooperator safe epoch
// computation.
//
ContextVar<State>& GetContextState();

} // end namespace coop::epoch
} // end namespace coop
