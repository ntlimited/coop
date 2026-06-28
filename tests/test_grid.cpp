#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/context.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/work/grid.h"

using namespace coop;
using Clock = std::chrono::steady_clock;

static inline uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
static inline void BusyFor(uint64_t ns)
{
    if (!ns) return;
    const uint64_t s = NowNs();
    volatile uint64_t x = 0;
    while (NowNs() - s < ns) for (int i = 0; i < 64; i++) x = x + (uint64_t)i;
    (void)x;
}

// Clustered shed (all Ergs shed from one cooperator -> land on one shard) must be drained by the
// whole grid via stealing: every Erg runs exactly once, and more than one cooperator participates.
//
TEST(GridTest, ShedBalancesAcrossCooperators)
{
    const int M = 4, N = 600;
    std::vector<Cooperator*> coops(M);
    std::vector<Thread*> threads(M);
    for (int m = 0; m < M; m++) { coops[m] = new Cooperator(); threads[m] = new Thread(coops[m]); }

    work::Grid grid;
    grid.Init(M);
    for (int m = 0; m < M; m++) grid.Join(coops[m]);

    std::atomic<int> remaining{N};
    std::vector<std::atomic<int>> ranOn(M);
    for (int m = 0; m < M; m++) ranOn[m].store(0, std::memory_order_relaxed);

    // Shed all N from cooperator 0; they cluster on shard 0 and must be stolen to balance.
    //
    coops[0]->Submit([&](Context*)
    {
        for (int i = 0; i < N; i++)
        {
            Shed([&]
            {
                BusyFor(5000);                               // heavy enough to force stealing
                Cooperator* me = GetCooperator();
                for (int m = 0; m < M; m++)
                    if (coops[m] == me) { ranOn[m].fetch_add(1, std::memory_order_relaxed); break; }
                remaining.fetch_sub(1, std::memory_order_acq_rel);
            });
        }
    });

    for (int spins = 0; remaining.load(std::memory_order_acquire) > 0 && spins < 200000; spins++)
        std::this_thread::sleep_for(std::chrono::microseconds(100));

    for (int m = 0; m < M; m++) coops[m]->Shutdown();
    for (int m = 0; m < M; m++) { delete threads[m]; delete coops[m]; }

    int total = 0, used = 0;
    for (int m = 0; m < M; m++) { int r = ranOn[m].load(std::memory_order_relaxed); total += r; if (r > 0) used++; }
    EXPECT_EQ(total, N) << "every shed Erg must run exactly once";
    EXPECT_GT(used, 1)  << "work-stealing must spread clustered Ergs across more than one cooperator";
}
