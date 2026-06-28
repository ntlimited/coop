// Standalone investigation bench for the Grid stealer self-wake (local-Shed doorbell).
//
// Measures the three axes the doorbell is meant to move:
//
//   pickup   -- latency from a local Shed to the Erg actually running, on a cooperator whose
//               own stealer has gone idle and backed its recheck timer off to the cap. This is
//               the hole the doorbell closes: without it the shed work waits out the backed-off
//               timer; with it the work is drained on the next scheduler pass.
//   makespan -- wall time to drain a clustered burst (all Ergs shed from one cooperator, stolen
//               across the grid). The doorbell must not regress this.
//   idle     -- stealer timer wakeups per second per core with no work at all. The doorbell must
//               fire only on real local sheds, so a fully idle grid must wake no more than the
//               adaptive-backoff baseline.
//
// Not built by CMake. Build standalone against a matching libcoop.a (headers and lib must be the
// same revision -- Participation layout differs between baseline and doorbell builds):
//
//   g++ -O2 -DNDEBUG -DCOOP_PERF_MODE=2 -msse4.2 -std=c++20 \
//       -I<coop_root> -I<coop_root>/build/release/_deps/spdlog-src/include \
//       benchmarks/grid_selfwake_bench.cpp <coop_root>/build/release/libcoop.a \
//       <coop_root>/build/release/_deps/spdlog-build/libspdlog.a -luring -lpthread -o grid_bench
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"
#include "coop/work/grid.h"

using namespace coop;
using Clock = std::chrono::steady_clock;

static inline uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

static inline void BusyFor(uint64_t ns)
{
    if (!ns) return;
    const uint64_t s = NowNs();
    volatile uint64_t x = 0;
    while (NowNs() - s < ns)
        for (int i = 0; i < 64; i++) x = x + (uint64_t)i;
    (void)x;
}

static void PrintStats(const char* label, std::vector<uint64_t>& v)
{
    if (v.empty()) { printf("  %-10s (no samples)\n", label); return; }
    std::sort(v.begin(), v.end());
    uint64_t sum = 0;
    for (auto s : v) sum += s;
    auto pct = [&](double p) { return v[(size_t)(p * (v.size() - 1))]; };
    printf("  %-10s n=%zu  mean=%.2fus  p50=%.2fus  p90=%.2fus  p99=%.2fus  max=%.2fus\n",
           label, v.size(), (double)sum / v.size() / 1000.0, pct(0.50) / 1000.0,
           pct(0.90) / 1000.0, pct(0.99) / 1000.0, pct(0.99999) / 1000.0);
}

// Axis 1: local-shed-then-idle pickup latency. Single-cooperator grid so the only consumer is the
// local stealer (no peer can steal the Erg first), isolating the doorbell's effect.
//
static void RunPickup(int iters, int warmup)
{
    Cooperator co;
    Thread th(&co);

    work::Grid grid;
    grid.Init(1);
    grid.Join(&co);

    std::vector<uint64_t> samples;
    samples.reserve(iters);
    std::atomic<bool> done{false};
    uint64_t pickupNs = 0;

    co.Submit([&](Context* ctx) {
        for (int i = 0; i < warmup + iters; i++)
        {
            // Let the stealer go fully idle and coast its recheck interval to the cap.
            //
            time::Sleep(ctx, std::chrono::milliseconds(5));

            Coordinator ran(ctx);                 // held; the Erg releases it when it runs
            uint64_t t0 = NowNs();
            Shed([&] {
                pickupNs = NowNs() - t0;
                ran.Release(Self());
            });
            ran.Acquire(ctx);                     // block until the stealer drains the Erg

            if (i >= warmup) samples.push_back(pickupNs);
        }
        done.store(true, std::memory_order_release);
    });

    while (!done.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    co.Shutdown();

    printf("[pickup] single-cooperator grid, shed-then-idle at recheck cap:\n");
    PrintStats("pickup", samples);
}

// Axis 2: clustered makespan. All Ergs shed from cooperator 0, drained by the whole grid through
// stealing. The doorbell must not regress this.
//
static void RunMakespan(int M, int N, uint64_t ergNs, int reps)
{
    std::vector<uint64_t> makespans;
    for (int r = 0; r < reps; r++)
    {
        std::vector<Cooperator*> coops(M);
        std::vector<Thread*> threads(M);
        for (int m = 0; m < M; m++) { coops[m] = new Cooperator(); threads[m] = new Thread(coops[m]); }

        work::Grid grid;
        grid.Init(M);
        for (int m = 0; m < M; m++) grid.Join(coops[m]);

        std::atomic<int> remaining{N};
        std::atomic<uint64_t> startNs{0};
        std::atomic<uint64_t> endNs{0};

        coops[0]->Submit([&](Context*) {
            startNs.store(NowNs(), std::memory_order_relaxed);
            for (int i = 0; i < N; i++)
            {
                Shed([&] {
                    BusyFor(ergNs);
                    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                        endNs.store(NowNs(), std::memory_order_relaxed);
                });
            }
        });

        for (int spins = 0; remaining.load(std::memory_order_acquire) > 0 && spins < 200000; spins++)
            std::this_thread::sleep_for(std::chrono::microseconds(50));

        for (int m = 0; m < M; m++) coops[m]->Shutdown();
        for (int m = 0; m < M; m++) { delete threads[m]; delete coops[m]; }

        makespans.push_back(endNs.load() - startNs.load());
    }
    std::sort(makespans.begin(), makespans.end());
    printf("[makespan] M=%d N=%d ergNs=%llu reps=%d:\n", M, N, (unsigned long long)ergNs, reps);
    printf("  min=%.3fms  median=%.3fms\n",
           makespans.front() / 1e6, makespans[makespans.size() / 2] / 1e6);
}

// Axis 3: idle stealer wakeup rate. M cooperators join, no work is ever shed, the grid sits idle
// for durationMs. Parks() counts idle timer arms (one per idle-core wakeup).
//
static void RunIdle(int M, int durationMs)
{
    std::vector<Cooperator*> coops(M);
    std::vector<Thread*> threads(M);
    for (int m = 0; m < M; m++) { coops[m] = new Cooperator(); threads[m] = new Thread(coops[m]); }

    work::Grid grid;
    grid.Init(M);
    for (int m = 0; m < M; m++) grid.Join(coops[m]);

    std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));
    uint64_t parks = grid.Parks();

    for (int m = 0; m < M; m++) coops[m]->Shutdown();
    for (int m = 0; m < M; m++) { delete threads[m]; delete coops[m]; }

    double secs = durationMs / 1000.0;
    printf("[idle] M=%d for %dms: parks=%llu  -> %.0f parks/s/core\n",
           M, durationMs, (unsigned long long)parks, parks / secs / M);
}

int main(int argc, char** argv)
{
    std::string mode = argc > 1 ? argv[1] : "all";

    if (mode == "pickup" || mode == "all")
        RunPickup(/*iters=*/300, /*warmup=*/20);
    if (mode == "makespan" || mode == "all")
        RunMakespan(/*M=*/4, /*N=*/600, /*ergNs=*/5000, /*reps=*/15);
    if (mode == "idle" || mode == "all")
        RunIdle(/*M=*/4, /*durationMs=*/2000);

    return 0;
}
