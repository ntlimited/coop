#pragma once

#include <cassert>
#include <cstdint>

namespace coop
{

struct Coordinator;

namespace time
{

// A node in a cooperator's TimerQueue, embedded directly in whatever owns the sleep (a Sleeper, the
// Grid stealer's park). Embedding is the point: registering a deadline allocates nothing, it is
// pointer surgery over memory the owner already holds. The node carries the absolute deadline and
// the Coordinator the queue Releases when that deadline expires.
//
// The three link pointers are the standard pairing-heap "child / sibling / back" triple:
//   m_child  first (leftmost) child, or null
//   m_next   next sibling in the parent's child list, or null
//   m_prev   previous sibling, or -- for a leftmost child -- the parent; null for the root
//
struct TimerNode
{
    TimerNode() = default;

    // A node that is still linked into a queue when destroyed would corrupt the heap. Owners must
    // remove it first (the Sleeper destructor does). Debug-only.
    //
    ~TimerNode()
    {
        assert(!m_linked && "TimerNode destroyed while still queued");
    }

    bool Linked() const { return m_linked; }

    int64_t DeadlineUs() const { return m_deadlineUs; }

    Coordinator* GetCoordinator() const { return m_coord; }

  private:
    friend struct TimerQueue;

    int64_t      m_deadlineUs = 0;
    Coordinator* m_coord      = nullptr;

    TimerNode*   m_child = nullptr;
    TimerNode*   m_next  = nullptr;
    TimerNode*   m_prev  = nullptr;
    bool         m_linked = false;
};

// A per-cooperator, deadline-ordered structure of in-flight sleeps. Single-threaded: every method
// runs on the owning cooperator's thread, so there are no atomics and no synchronization. It is an
// intrusive pairing heap keyed by absolute monotonic microseconds -- O(1) insert and find-min,
// amortized O(log n) delete-min and arbitrary delete. See docs/timer_wheel_001.md for why a pairing
// heap over a red-black tree.
//
struct TimerQueue
{
    bool Empty() const { return m_root == nullptr; }

    // The nearest deadline in the queue. Only valid when !Empty().
    //
    int64_t MinDeadlineUs() const
    {
        assert(m_root);
        return m_root->m_deadlineUs;
    }

    // Register a node with the given absolute deadline and the coordinator to Release on expiry.
    // O(1).
    //
    void Insert(TimerNode* node, int64_t deadlineUs, Coordinator* coord)
    {
        assert(!node->m_linked);
        node->m_deadlineUs = deadlineUs;
        node->m_coord      = coord;
        node->m_child = node->m_next = node->m_prev = nullptr;
        node->m_linked = true;
        m_root = Meld(m_root, node);
        m_root->m_prev = nullptr;
    }

    // Remove a node from the queue. Idempotent: a no-op if the node is not currently linked, which
    // is what makes cleanup paths (a killed sleep's destructor) safe to call unconditionally.
    //
    void Remove(TimerNode* node);

    // If the nearest deadline is at or before nowUs, remove and return that node; otherwise null.
    // The scheduler calls this in a loop to drain everything that has come due.
    //
    TimerNode* PopExpired(int64_t nowUs)
    {
        if (!m_root || m_root->m_deadlineUs > nowUs)
        {
            return nullptr;
        }
        TimerNode* min = m_root;
        Remove(min);
        return min;
    }

    // Heap-property check: every node's deadline is <= its children's. A test/debug helper, not used
    // on any hot path.
    //
    bool Validate() const { return Validate(m_root); }

  private:
    // Make the higher-keyed root a child of the lower-keyed root; returns the new root. Equal keys
    // are fine -- a pairing heap needs no tie-break.
    //
    static TimerNode* Meld(TimerNode* a, TimerNode* b)
    {
        if (!a) return b;
        if (!b) return a;
        if (a->m_deadlineUs <= b->m_deadlineUs)
        {
            AddChild(a, b);
            return a;
        }
        AddChild(b, a);
        return b;
    }

    static void AddChild(TimerNode* parent, TimerNode* c)
    {
        c->m_prev = parent;
        c->m_next = parent->m_child;
        if (parent->m_child)
        {
            parent->m_child->m_prev = c;
        }
        parent->m_child = c;
    }

    // Two-pass merge of a sibling list into a single tree, iterative so a node with many children
    // cannot overflow the native stack.
    //
    static TimerNode* MergePairs(TimerNode* first);

    static bool Validate(const TimerNode* n);

    TimerNode* m_root = nullptr;
};

} // end namespace time
} // end namespace coop
