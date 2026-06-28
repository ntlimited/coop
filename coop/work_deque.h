#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace coop
{

// Bounded single-owner / multi-thief work-stealing deque (Chase-Lev, with the Le, Pop, Cohen &
// Nardelli 2013 memory ordering for weak memory models — correct on aarch64, not just x86 TSO).
//
// The owner pushes and pops its own end (the bottom): LIFO, so recently-shed work is cache-hot and
// the owner's path is uncontended in the common case. Thieves steal the far end (the top): FIFO, so
// they take the oldest task (likely the largest remaining unit of work). Exactly one consumer ever
// receives a given item — the last-element race between the owner's pop and a thief's steal is
// resolved by a CAS on the top index that only one side can win.
//
// Bounded (no growth): PushBottom returns false when full. Callers handle overflow out of band
// (run the task inline, or push to a shared injector) rather than the deque reallocating. T must be
// trivially copyable — a task pointer.
//
template<typename T, size_t CAP = 256>
class WorkDeque
{
    static_assert((CAP & (CAP - 1)) == 0, "CAP must be a power of two");
    static constexpr int64_t kMask = (int64_t)CAP - 1;

  public:
    WorkDeque() : m_top(0), m_bottom(0) {}
    WorkDeque(const WorkDeque&) = delete;
    WorkDeque& operator=(const WorkDeque&) = delete;

    // Owner only. Append to the bottom. Returns false if the deque is full.
    //
    bool PushBottom(T v)
    {
        const int64_t b = m_bottom.load(std::memory_order_relaxed);
        const int64_t t = m_top.load(std::memory_order_acquire);
        if (b - t >= (int64_t)CAP)
        {
            return false;                                       // full
        }
        m_buf[b & kMask] = v;
        std::atomic_thread_fence(std::memory_order_release);    // publish the slot before bottom
        m_bottom.store(b + 1, std::memory_order_relaxed);
        return true;
    }

    // Owner only. Take from the bottom (LIFO). Returns false if empty or if the last element was
    // stolen out from under us in the race.
    //
    bool PopBottom(T& out)
    {
        const int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
        m_bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);    // order bottom store vs top load
        int64_t t = m_top.load(std::memory_order_relaxed);

        if (t > b)
        {
            m_bottom.store(b + 1, std::memory_order_relaxed);   // empty: restore
            return false;
        }

        T v = m_buf[b & kMask];
        if (t == b)
        {
            // Last element: a thief may be reading the same slot. Whoever wins the CAS on top
            // takes it.
            //
            const bool won = m_top.compare_exchange_strong(
                t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
            m_bottom.store(b + 1, std::memory_order_relaxed);
            if (!won)
            {
                return false;                                   // a thief took it
            }
        }
        out = v;
        return true;
    }

    // Any thief. Take from the top (FIFO). Returns false on empty or on losing the steal race; the
    // caller simply tries another victim — the item is not lost, another consumer has it.
    //
    bool Steal(T& out)
    {
        int64_t t = m_top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);    // order top load vs bottom load
        const int64_t b = m_bottom.load(std::memory_order_acquire);
        if (t >= b)
        {
            return false;                                       // empty
        }
        T v = m_buf[t & kMask];
        if (!m_top.compare_exchange_strong(
                t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
        {
            return false;                                       // lost the race
        }
        out = v;
        return true;
    }

    // Owner's approximate view of the size. Exact when no steal is in flight; for idle/steal
    // heuristics only, never for correctness.
    //
    int64_t SizeApprox() const
    {
        const int64_t b = m_bottom.load(std::memory_order_relaxed);
        const int64_t t = m_top.load(std::memory_order_relaxed);
        const int64_t n = b - t;
        return n < 0 ? 0 : n;
    }

    bool IsEmptyApprox() const { return SizeApprox() == 0; }

  private:
    // top and bottom on separate cache lines so the owner's bottom writes do not false-share with
    // the thieves' top CAS.
    //
    alignas(64) std::atomic<int64_t> m_top;
    alignas(64) std::atomic<int64_t> m_bottom;
    alignas(64) T m_buf[CAP];
};

} // end namespace coop
