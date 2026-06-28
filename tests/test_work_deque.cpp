#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/work_deque.h"

using namespace coop;

namespace
{

// Item encoding: a small integer carried as the pointer-sized T.
//
static inline void* Item(int i) { return reinterpret_cast<void*>((intptr_t)(i + 1)); }
static inline int   Index(void* v) { return (int)((intptr_t)v) - 1; }

// Deliberately-broken deque for the red calibration: PopBottom takes the last element WITHOUT the
// CAS that resolves the owner/thief race, so the owner and a concurrent thief can both receive it.
// Everything else mirrors WorkDeque. The stress harness must catch the resulting duplication.
//
template<typename T, size_t CAP = 256>
class BrokenDeque
{
    static constexpr int64_t kMask = (int64_t)CAP - 1;
  public:
    BrokenDeque() : m_top(0), m_bottom(0) {}
    bool PushBottom(T v)
    {
        const int64_t b = m_bottom.load(std::memory_order_relaxed);
        const int64_t t = m_top.load(std::memory_order_acquire);
        if (b - t >= (int64_t)CAP) return false;
        m_buf[b & kMask] = v;
        std::atomic_thread_fence(std::memory_order_release);
        m_bottom.store(b + 1, std::memory_order_relaxed);
        return true;
    }
    bool PopBottom(T& out)
    {
        const int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
        m_bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const int64_t t = m_top.load(std::memory_order_relaxed);
        if (t > b) { m_bottom.store(b + 1, std::memory_order_relaxed); return false; }
        out = m_buf[b & kMask];
        if (t == b) { m_bottom.store(b + 1, std::memory_order_relaxed); }   // BUG: no CAS on top
        return true;
    }
    bool Steal(T& out)
    {
        int64_t t = m_top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const int64_t b = m_bottom.load(std::memory_order_acquire);
        if (t >= b) return false;
        T v = m_buf[t & kMask];
        if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            return false;
        out = v;
        return true;
    }
  private:
    alignas(64) std::atomic<int64_t> m_top;
    alignas(64) std::atomic<int64_t> m_bottom;
    alignas(64) T m_buf[CAP];
};

struct StressResult { bool anyZero; bool anyDup; bool stalled; };

// One owner (this thread: push/pop), `thieves` stealing threads. Produce `items` unique items, then
// everyone drains until all are consumed. Each item must be consumed exactly once.
//
template<typename Deque>
static StressResult RunStress(int items, int thieves)
{
    Deque dq;
    std::vector<std::atomic<int>> consumed(items);
    for (int i = 0; i < items; i++) consumed[i].store(0, std::memory_order_relaxed);
    std::atomic<int> total{0};

    auto record = [&](void* v) {
        consumed[Index(v)].fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(1, std::memory_order_acq_rel);
    };

    std::vector<std::thread> thiefs;
    for (int k = 0; k < thieves; k++)
        thiefs.emplace_back([&] {
            void* v;
            while (total.load(std::memory_order_acquire) < items)
                if (dq.Steal(v)) record(v);
        });

    int produced = 0;
    void* v;
    uint64_t spins = 0;
    const uint64_t kCap = 2000ull * 1000 * 1000;        // safety: bail rather than hang on under-count
    while (produced < items)
    {
        if (dq.PushBottom(Item(produced))) { produced++; }
        else if (dq.PopBottom(v)) { record(v); }        // full: make room
        if (++spins > kCap) break;
    }
    while (total.load(std::memory_order_acquire) < items)
    {
        if (dq.PopBottom(v)) record(v);
        if (++spins > kCap) break;
    }

    for (auto& t : thiefs) t.join();

    StressResult r{false, false, spins > kCap};
    for (int i = 0; i < items; i++)
    {
        int c = consumed[i].load(std::memory_order_relaxed);
        if (c == 0) r.anyZero = true;
        if (c > 1) r.anyDup = true;
    }
    return r;
}

} // namespace

// Owner end is LIFO.
//
TEST(WorkDequeTest, OwnerLifo)
{
    WorkDeque<void*> dq;
    for (int i = 0; i < 8; i++) EXPECT_TRUE(dq.PushBottom(Item(i)));
    void* v;
    for (int i = 7; i >= 0; i--) { ASSERT_TRUE(dq.PopBottom(v)); EXPECT_EQ(Index(v), i); }
    EXPECT_FALSE(dq.PopBottom(v));
}

// Thief end is FIFO.
//
TEST(WorkDequeTest, StealFifo)
{
    WorkDeque<void*> dq;
    for (int i = 0; i < 8; i++) dq.PushBottom(Item(i));
    void* v;
    for (int i = 0; i < 8; i++) { ASSERT_TRUE(dq.Steal(v)); EXPECT_EQ(Index(v), i); }
    EXPECT_FALSE(dq.Steal(v));
}

// PushBottom fails when full.
//
TEST(WorkDequeTest, FullRejects)
{
    WorkDeque<void*, 4> dq;
    for (int i = 0; i < 4; i++) EXPECT_TRUE(dq.PushBottom(Item(i)));
    EXPECT_FALSE(dq.PushBottom(Item(4)));
}

// Green: under one owner + three thieves, every item is consumed exactly once.
//
TEST(WorkDequeTest, ConcurrentExactlyOnce)
{
    for (int rep = 0; rep < 20; rep++)
    {
        StressResult r = RunStress<WorkDeque<void*>>(200000, 3);
        ASSERT_FALSE(r.stalled) << "rep " << rep << ": under-count stall (lost item)";
        EXPECT_FALSE(r.anyZero) << "rep " << rep << ": an item was never consumed";
        EXPECT_FALSE(r.anyDup)  << "rep " << rep << ": an item was consumed twice";
    }
}

// Red calibration: the same harness detects the broken deque's duplication. Proves the stress test
// has detection power (red/green), not just that the correct deque happens to pass.
//
TEST(WorkDequeTest, StressCatchesBrokenDeque)
{
    bool detected = false;
    for (int rep = 0; rep < 50 && !detected; rep++)
    {
        StressResult r = RunStress<BrokenDeque<void*>>(200000, 3);
        if (r.anyDup || r.anyZero || r.stalled) detected = true;
    }
    EXPECT_TRUE(detected) << "stress harness failed to catch the broken (no last-element CAS) deque";
}
