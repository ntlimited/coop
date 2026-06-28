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

// A caller-owned Erg (m_stealerOwned=false) shed via Shed(Erg*) -- the zero-allocation companion to
// Shed(fn) -- must be run by the grid and NOT freed by the stealer: the caller keeps ownership so a
// stable pipeline can re-shed the same object. We verify both the run and the no-free: the Erg sets
// a sentinel during Run, and after shutdown the caller still reads its state (a freed object would
// trip the debug guard pages). All Ergs are stack-resident in the test frame; none are allocated per
// shed.
//
TEST(GridTest, ShedReusableErgNotFreed)
{
    struct OwnedErg final : work::Erg
    {
        std::atomic<int> runs{0};
        OwnedErg() { m_stealerOwned = false; }
        void Run() final { runs.fetch_add(1, std::memory_order_relaxed); }
    };

    const int M = 4, N = 400;
    std::vector<Cooperator*> coops(M);
    std::vector<Thread*> threads(M);
    for (int m = 0; m < M; m++) { coops[m] = new Cooperator(); threads[m] = new Thread(coops[m]); }

    work::Grid grid;
    grid.Init(M);
    for (int m = 0; m < M; m++) grid.Join(coops[m]);

    std::vector<OwnedErg> ergs(N);
    std::atomic<int> remaining{N};

    coops[0]->Submit([&](Context*)
    {
        for (int i = 0; i < N; i++)
        {
            // Shed our own long-lived Erg; the stealer runs it without freeing it.
            //
            bool ok = Shed(&ergs[i]);
            EXPECT_TRUE(ok) << "Shed(Erg*) must accept a caller-owned Erg on a participating cooperator";
            (void)ok;
        }
    });

    // Drain: each Erg runs exactly once; count completions by polling the per-Erg sentinel.
    //
    for (int spins = 0; spins < 200000; spins++)
    {
        int done = 0;
        for (int i = 0; i < N; i++) if (ergs[i].runs.load(std::memory_order_relaxed) > 0) done++;
        if (done == N) break;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    for (int m = 0; m < M; m++) coops[m]->Shutdown();
    for (int m = 0; m < M; m++) { delete threads[m]; delete coops[m]; }

    // The caller still owns every Erg (not freed by the stealer) and each ran exactly once.
    //
    for (int i = 0; i < N; i++)
        EXPECT_EQ(ergs[i].runs.load(std::memory_order_relaxed), 1)
            << "caller-owned Erg " << i << " must run exactly once and survive (not be freed)";
}
