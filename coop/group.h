#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <type_traits>
#include <utility>

#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/semaphore.h"
#include "coop/spawn_configuration.h"

namespace coop
{

// Group: a structured-concurrency scope. Go() spawns a child under the group; the
// destructor (or an explicit Wait()) blocks until every child has finished, so children
// never outlive the scope that launched them. This is coop's errgroup / JoinSet / gate:
//
//   - Join: Wait() blocks until all children retire and returns whether all succeeded.
//   - Fail-fast: the first child that reports failure cancels its siblings (Handle::Kill,
//     which children observe through their kill-aware waits) and makes Wait() return
//     false. Later failures are subsumed.
//   - Bounded fan-out: SetLimit(n) gates admission through a Semaphore, so Go() blocks
//     the owner when n children are already in flight — backpressure without a
//     hand-rolled counter. The permit rides into the child and frees on its exit.
//
// Complementary to kill trees rather than redundant: a kill tree propagates termination
// downward; a Group drains upward (waits) and can refuse/cancel work. Single-cooperator:
// the owner and all children live on one cooperator; no atomics.
//
// Zero cost when unused: a Group is a few words plus empty containers; the Semaphore and
// its machinery exist only when SetLimit is called. Nothing is paid by code that never
// constructs one.
//
// A child function returns bool (true = success) or void (always successful). It runs on
// its own context and must be kill-aware (CoordinateWithKill / *Kill IO) for
// cancellation to reach it — the same contract as every coop blocking call.
//
struct Group
{
    explicit Group(Context* owner)
    : m_owner(owner)
    , m_cooperator(owner->GetCooperator())
    {
    }

    Group(Group const&) = delete;
    Group& operator=(Group const&) = delete;

    ~Group() { Wait(); }

    // Bound the number of children in flight. Call before the first Go(). A limit of 0
    // is treated as unbounded.
    //
    void SetLimit(size_t maxInFlight)
    {
        if (maxInFlight > 0)
        {
            m_limit.emplace(maxInFlight);
        }
    }

    // Spawn a child running fn. With a limit set, blocks the owner until a slot frees
    // (kill-aware — returns false if the owner is killed while waiting). Returns false
    // without spawning if the group is already failing or the spawn fails.
    //
    template<typename Fn>
    bool Go(Fn&& fn)
    {
        return Go(SpawnConfiguration{}, std::forward<Fn>(fn));
    }

    template<typename Fn>
    bool Go(SpawnConfiguration const& config, Fn&& fn);

    // Block until every child has finished. Returns true if all succeeded, false if any
    // failed (siblings were cancelled). Idempotent — safe to call explicitly and again
    // from the destructor.
    //
    bool Wait();

    bool Failed() const { return m_failed; }
    size_t InFlight() const { return m_live; }

  private:
    void OnChildDone(bool ok);

    Context*                    m_owner;
    Cooperator*                 m_cooperator;
    Coordinator                 m_done;        // held while m_live > 0; wakes Wait at 0
    std::optional<Semaphore>    m_limit;
    std::deque<Context::Handle> m_handles;     // stable addresses; Handle is non-movable
    size_t                      m_live = 0;
    bool                        m_failed = false;
    bool                        m_cancelling = false;
};

template<typename Fn>
bool Group::Go(SpawnConfiguration const& config, Fn&& fn)
{
    if (m_failed)
    {
        return false;
    }

    // Bounded fan-out: acquire a slot (may block the owner). The permit moves into the
    // child and frees on the child's exit — the units-into-a-child pattern Permit is for.
    //
    Permit permit;
    if (m_limit)
    {
        permit = m_limit->AcquireKill(m_owner);
        if (!permit)
        {
            return false;   // owner killed while waiting for a slot
        }
    }

    if (m_live == 0)
    {
        m_done.TryAcquire(m_owner);   // hold while children are live
    }
    m_live++;

    // Handle storage is a deque so existing handles never relocate (Handle is
    // non-movable) and a finished child's stale handle stays a safe Kill target.
    //
    Context::Handle& handle = m_handles.emplace_back();

    bool spawned = m_cooperator->Spawn(config,
        [this, fn = std::forward<Fn>(fn), permit = std::move(permit)](Context* c) mutable
        {
            bool ok;
            if constexpr (std::is_same_v<std::invoke_result_t<Fn, Context*>, bool>)
            {
                ok = fn(c);
            }
            else
            {
                fn(c);
                ok = true;
            }
            OnChildDone(ok);
            // permit frees here
        },
        &handle);

    if (!spawned)
    {
        m_live--;
        m_handles.pop_back();
        if (m_live == 0 && m_done.IsHeld())
        {
            m_done.Release(m_owner, false);
        }
        return false;
    }
    return true;
}

} // end namespace coop
