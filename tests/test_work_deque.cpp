#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/work/detail/deque.h"

using namespace coop;

namespace
{

// Item encoding: a small integer carried as the pointer-sized T.
//
static inline void* Item(int i) { return reinterpret_cast<void*>((intptr_t)(i + 1)); }
static inline int   Index(void* v) { return (int)((intptr_t)v) - 1; }

// Deliberately-broken deque for the red calibration. It mirrors the production deque except that
// the owner does not advance top after taking the last element.
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
        int64_t t = m_top.load(std::memory_order_relaxed);
        if (t > b)
        {
            m_bottom.store(b + 1, std::memory_order_relaxed);
            return false;
        }

        T v = m_buf[b & kMask];
        if (t == b)
        {
            // BUG: omit only the last-element CAS/top advancement.
            m_bottom.store(b + 1, std::memory_order_relaxed);
        }
        out = v;
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

struct ExactOnceResult
{
    bool missing;
    bool duplicate;
    bool invalidItem;
    bool stalled;
};

class ExactOnceLedger
{
  public:
    explicit ExactOnceLedger(size_t items) : m_counts(items)
    {
        for (auto& count : m_counts) count.store(0, std::memory_order_relaxed);
    }

    void Record(void* item)
    {
        const uintptr_t encoded = reinterpret_cast<uintptr_t>(item);
        if (encoded == 0 || encoded > m_counts.size())
        {
            m_invalidItem.store(true, std::memory_order_relaxed);
            return;
        }
        m_counts[static_cast<size_t>(encoded - 1)].fetch_add(1, std::memory_order_relaxed);
    }

    ExactOnceResult Result(bool stalled) const
    {
        ExactOnceResult result{false, false, m_invalidItem.load(std::memory_order_relaxed), stalled};
        for (const auto& count : m_counts)
        {
            const int value = count.load(std::memory_order_relaxed);
            if (value == 0) result.missing = true;
            if (value > 1) result.duplicate = true;
        }
        return result;
    }

  private:
    std::vector<std::atomic<int>> m_counts;
    std::atomic<bool>             m_invalidItem{false};
};

// One owner (this thread: push/pop), `thieves` stealing threads. Produce `items` unique items, then
// everyone drains until all are consumed. Each item must be consumed exactly once.
template<typename Deque>
static ExactOnceResult RunStress(int items, int thieves)
{
    Deque dq;
    ExactOnceLedger ledger(items);
    std::atomic<bool> stopThieves{false};

    std::vector<std::thread> thiefs;
    for (int k = 0; k < thieves; k++)
        thiefs.emplace_back([&] {
            void* v;
            while (!stopThieves.load(std::memory_order_acquire))
                if (dq.Steal(v)) ledger.Record(v);
        });

    int produced = 0;
    void* v;
    // Each item needs a push and can need an owner pop; 64 attempts per item leaves generous room
    // for concurrent steal races without a fixed, operationally absurd cap.
    const uint64_t kOperationBudget = static_cast<uint64_t>(items) * 64 + 1024;
    uint64_t productionOperations = 0;
    while (produced < items && productionOperations < kOperationBudget)
    {
        productionOperations++;
        if (dq.PushBottom(Item(produced))) { produced++; }
        else if (dq.PopBottom(v)) { ledger.Record(v); } // full: make room
    }

    bool stalled = produced != items;
    uint64_t drainOperations = 0;
    while (!stalled && drainOperations < kOperationBudget)
    {
        drainOperations++;
        if (!dq.PopBottom(v)) break;
        ledger.Record(v);
    }

    if (drainOperations == kOperationBudget) stalled = true;

    stopThieves.store(true, std::memory_order_release);
    for (auto& t : thiefs) t.join();

    return ledger.Result(stalled);
}

template<typename Deque>
static ExactOnceResult ProbeLastElement()
{
    Deque dq;
    ExactOnceLedger ledger(1);
    void* v;

    if (dq.PushBottom(Item(0)))
    {
        if (dq.PopBottom(v)) ledger.Record(v);
        if (dq.Steal(v)) ledger.Record(v);
    }
    return ledger.Result(false);
}

} // namespace

// Owner end is LIFO.
//
TEST(WorkDequeTest, OwnerLifo)
{
    work::detail::Deque<void*> dq;
    for (int i = 0; i < 8; i++) EXPECT_TRUE(dq.PushBottom(Item(i)));
    void* v;
    for (int i = 7; i >= 0; i--) { ASSERT_TRUE(dq.PopBottom(v)); EXPECT_EQ(Index(v), i); }
    EXPECT_FALSE(dq.PopBottom(v));
}

// Thief end is FIFO.
//
TEST(WorkDequeTest, StealFifo)
{
    work::detail::Deque<void*> dq;
    for (int i = 0; i < 8; i++) dq.PushBottom(Item(i));
    void* v;
    for (int i = 0; i < 8; i++) { ASSERT_TRUE(dq.Steal(v)); EXPECT_EQ(Index(v), i); }
    EXPECT_FALSE(dq.Steal(v));
}

// PushBottom fails when full.
//
TEST(WorkDequeTest, FullRejects)
{
    work::detail::Deque<void*, 4> dq;
    for (int i = 0; i < 4; i++) EXPECT_TRUE(dq.PushBottom(Item(i)));
    EXPECT_FALSE(dq.PushBottom(Item(4)));
}

// Green: under one owner + three thieves, every item is consumed exactly once.
//
TEST(WorkDequeTest, ConcurrentExactlyOnce)
{
    for (int rep = 0; rep < 20; rep++)
    {
        ExactOnceResult r = RunStress<work::detail::Deque<void*>>(200000, 3);
        ASSERT_FALSE(r.stalled) << "rep " << rep << ": operation budget exhausted";
        EXPECT_FALSE(r.missing) << "rep " << rep << ": an item was never consumed";
        EXPECT_FALSE(r.duplicate) << "rep " << rep << ": an item was consumed twice";
        EXPECT_FALSE(r.invalidItem) << "rep " << rep << ": deque returned an invalid item";
    }
}

// Red/green calibration of the last-element ownership rule without scheduler-dependent timing.
//
TEST(WorkDequeTest, LastElementCalibrationCatchesBrokenDeque)
{
    ExactOnceResult real = ProbeLastElement<work::detail::Deque<void*>>();
    EXPECT_FALSE(real.missing);
    EXPECT_FALSE(real.duplicate);
    EXPECT_FALSE(real.invalidItem);
    EXPECT_FALSE(real.stalled);

    ExactOnceResult broken = ProbeLastElement<BrokenDeque<void*>>();
    EXPECT_TRUE(broken.duplicate);
    EXPECT_FALSE(broken.missing);
    EXPECT_FALSE(broken.invalidItem);
    EXPECT_FALSE(broken.stalled);
}
