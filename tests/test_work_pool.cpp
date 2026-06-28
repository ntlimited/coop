#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/work/detail/shards.h"
#include "coop/work/erg.h"

using namespace coop;

// Single worker: shed locally, pull locally.
//
TEST(WorkPoolTest, LocalShedPull)
{
    work::detail::Shards<work::Erg*> pool;
    pool.Init(1);
    int runs = 0;
    for (int i = 0; i < 16; i++) ASSERT_TRUE(pool.Shed(0, work::MakeErg([&] { runs++; })));
    while (work::Erg* t = pool.Pull(0)) work::RunErg(t);
    EXPECT_EQ(runs, 16);
}

// Clustered seed (all work shed to shard 0) must be drained by ALL workers via stealing, with every
// task run exactly once. This is the balancing the substrate exists for, at the mechanism level.
//
TEST(WorkPoolTest, StealBalancesClusteredSeed)
{
    const int M = 4, N = 4000;
    for (int rep = 0; rep < 10; rep++)
    {
        work::detail::Shards<work::Erg*> pool;
        pool.Init(M);
        std::vector<std::atomic<int>> ran(N);
        for (int i = 0; i < N; i++) ran[i].store(0, std::memory_order_relaxed);
        std::atomic<int> remaining{N};
        std::atomic<bool> seeded{false};

        std::vector<std::thread> workers;
        for (int w = 0; w < M; w++)
            workers.emplace_back([&, w] {
                if (w == 0)
                {
                    // Worker 0 owns shard 0 — it must be the one that sheds (single-owner).
                    //
                    for (int i = 0; i < N; i++)
                    {
                        work::Erg* t = work::MakeErg([&ran, i] { ran[i].fetch_add(1, std::memory_order_relaxed); });
                        while (!pool.Shed(0, t)) { /* never full: N < CAP */ }
                    }
                    seeded.store(true, std::memory_order_release);
                }
                else
                {
                    while (!seeded.load(std::memory_order_acquire)) std::this_thread::yield();
                }
                while (remaining.load(std::memory_order_acquire) > 0)
                {
                    if (work::Erg* t = pool.Pull(w)) { work::RunErg(t); remaining.fetch_sub(1, std::memory_order_acq_rel); }
                }
            });
        for (auto& t : workers) t.join();

        for (int i = 0; i < N; i++)
            ASSERT_EQ(ran[i].load(std::memory_order_relaxed), 1) << "rep " << rep << " task " << i;
    }
}

// A reusable (caller-owned) Erg re-sheds the SAME object each stage instead of allocating a fresh
// morsel: the substrate's headline re-shedding pipeline with zero per-stage allocation. The
// single-owner contract is that a reusable Erg is never resident in a shard and running at once --
// here the re-shed happens only AFTER RunErg returns (the stage's work has retired), so the object
// is idle when it re-enters a shard. Under real concurrent stealing every stage must run exactly
// once, and no two stealers may run the same Erg concurrently. RunErg must not free it (it is
// caller-owned), so the objects remain valid and the per-stage allocation count is zero.
//
TEST(WorkPoolTest, ReusableErgReshedSingleOwner)
{
    struct Stage final : work::Erg
    {
        work::detail::Shards<work::Erg*>* shards = nullptr;
        int                               stages = 0;
        int                               runs   = 0;
        std::atomic<int>                  inRun{0};
        std::atomic<bool>*                doubleRun = nullptr;

        Stage() { m_stealerOwned = false; }   // caller-owned: the stealer must not free us

        void Run() final
        {
            if (inRun.fetch_add(1, std::memory_order_acq_rel) != 0)
                doubleRun->store(true, std::memory_order_relaxed);
            ++runs;
            --stages;
            inRun.fetch_sub(1, std::memory_order_acq_rel);
        }
    };

    const int M = 4, N = 1500, K = 8;
    for (int rep = 0; rep < 10; rep++)
    {
        work::detail::Shards<work::Erg*> pool;
        pool.Init(M);
        std::vector<Stage> stages(N);
        std::atomic<int>   remaining{N};
        std::atomic<bool>  doubleRun{false};
        std::atomic<bool>  seeded{false};

        std::vector<std::thread> workers;
        for (int w = 0; w < M; w++)
            workers.emplace_back([&, w] {
                if (w == 0)
                {
                    for (int i = 0; i < N; i++)
                    {
                        stages[i].shards = &pool; stages[i].stages = K; stages[i].doubleRun = &doubleRun;
                        while (!pool.Shed(0, &stages[i])) {}   // clustered seed on shard 0
                    }
                    seeded.store(true, std::memory_order_release);
                }
                else
                {
                    while (!seeded.load(std::memory_order_acquire)) std::this_thread::yield();
                }
                while (remaining.load(std::memory_order_acquire) > 0)
                {
                    work::Erg* e = pool.Pull(w);
                    if (!e) continue;
                    Stage* s = static_cast<Stage*>(e);
                    work::RunErg(s);                           // runs, does NOT free (caller-owned)
                    // Re-shed only now that Run() has retired -> the object is idle, single-owner holds.
                    //
                    if (s->stages > 0) { while (!pool.Shed(w, s)) {} }
                    else remaining.fetch_sub(1, std::memory_order_acq_rel);
                }
            });
        for (auto& t : workers) t.join();

        ASSERT_FALSE(doubleRun.load()) << "two stealers ran the same reusable Erg concurrently (rep "
                                       << rep << ")";
        for (int i = 0; i < N; i++)
            ASSERT_EQ(stages[i].runs, K) << "rep " << rep << " pipeline " << i
                                         << " did not run every stage exactly once";
    }
}
