#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/work_pool.h"

using namespace coop;

// Single worker: shed locally, pull locally.
//
TEST(WorkPoolTest, LocalShedPull)
{
    WorkPool<> pool;
    pool.Init(1);
    int runs = 0;
    for (int i = 0; i < 16; i++) ASSERT_TRUE(pool.Shed(0, MakeTask([&] { runs++; })));
    while (Task* t = pool.Pull(0)) RunTask(t);
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
        WorkPool<> pool;
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
                        Task* t = MakeTask([&ran, i] { ran[i].fetch_add(1, std::memory_order_relaxed); });
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
                    if (Task* t = pool.Pull(w)) { RunTask(t); remaining.fetch_sub(1, std::memory_order_acq_rel); }
                }
            });
        for (auto& t : workers) t.join();

        for (int i = 0; i < N; i++)
            ASSERT_EQ(ran[i].load(std::memory_order_relaxed), 1) << "rep " << rep << " task " << i;
    }
}
