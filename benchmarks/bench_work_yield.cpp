// Worker-loop yield-tax harness.
//
// The question: with directYield, how close does a *cooperative* coop worker loop
// ([work -> yield] repeated) get to a *pure* worker loop ([work] repeated, no scheduler)? The gap
// is the cooperative-yield tax, paid once per work unit. The pure-loop ceiling is the same work with
// the yields removed; the tax is (cooperative makespan - pure makespan).
//
// Unlike the zero-work yield microbench, every unit does real, tunable compute -- because a count
// budget only maps to a wall-clock poll interval once there is work between switches, and both
// governor pathologies are about work:
//   - contexts-per-cooperator is the runq length: a SHORT list (2-4) cycled many times vs a LONG
//     list (64-256). The short-list/expensive case is where a count budget can defer bookkeeping for
//     a long wall-clock.
//   - --work is the per-unit cost: CHEAP (sub-us) vs EXPENSIVE (multi-us).
//
// directYield off vs on vs --pure isolates the mechanism. M cooperators run independently (default
// 1, to isolate the in-cooperator fastpath from cross-thread noise).
//
// Usage:
//   bench_work_yield [--coops=1] [--ctxs=8] [--iters=10000] [--work=500]
//                    [--direct] [--budget=64] [--pure] [--nopin]
//
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/self.h"
#include "coop/thread.h"

using Clock = std::chrono::steady_clock;

static inline uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

// Spin for approximately `ns` of compute. The volatile sink defeats dead-code elimination so the
// work is real; the inner unrolled loop keeps the NowNs() polling rate from dominating short spans.
//
static inline void BusyFor(uint64_t ns)
{
    if (!ns) return;
    const uint64_t start = NowNs();
    volatile uint64_t sink = 0;
    while (NowNs() - start < ns)
        for (int i = 0; i < 64; i++) sink = sink + (uint64_t)i * 2654435761u;
    (void)sink;
}

static uint32_t g_coops = 1, g_ctxs = 8, g_budget = 64;
static uint64_t g_iters = 10000, g_work = 500;
static bool g_direct = false, g_pure = false;

int main(int argc, char** argv)
{
    bool pin = true;
    for (int i = 1; i < argc; i++)
    {
        auto eq = [&](const char* k){ return strncmp(argv[i], k, strlen(k)) == 0; };
        if (eq("--coops=")) g_coops = atoi(argv[i] + 8);
        else if (eq("--ctxs=")) g_ctxs = atoi(argv[i] + 7);
        else if (eq("--iters=")) g_iters = strtoull(argv[i] + 8, nullptr, 10);
        else if (eq("--work=")) g_work = strtoull(argv[i] + 7, nullptr, 10);
        else if (eq("--budget=")) g_budget = atoi(argv[i] + 9);
        else if (eq("--direct")) g_direct = true;
        else if (eq("--pure")) g_pure = true;
        else if (eq("--nopin")) pin = false;
    }

    const int M = (int)g_coops;
    const int totalCtxs = M * (int)g_ctxs;
    std::atomic<int> remaining{totalCtxs};

    std::vector<coop::Cooperator*> coops(M);
    std::vector<coop::Thread*> threads(M);
    for (int i = 0; i < M; i++)
    {
        coop::CooperatorConfiguration cfg;
        cfg.directYield = g_direct;
        cfg.directYieldBudget = (int)g_budget;
        coops[i] = new coop::Cooperator(cfg);
        threads[i] = new coop::Thread(coops[i]);
        if (pin) threads[i]->PinToCore(i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    const uint64_t iters = g_iters, work = g_work;
    const bool pure = g_pure;
    const uint64_t t0 = NowNs();

    for (int c = 0; c < M; c++)
    {
        coops[c]->Submit([&remaining, iters, work, pure](coop::Context*)
        {
            for (uint32_t k = 0; k < g_ctxs; k++)
            {
                coop::Spawn([&remaining, iters, work, pure](coop::Context* ctx)
                {
                    ctx->Detach();
                    for (uint64_t i = 0; i < iters; i++)
                    {
                        BusyFor(work);
                        if (!pure) ctx->Yield(true);
                    }
                    remaining.fetch_sub(1, std::memory_order_acq_rel);
                });
            }
        });
    }

    while (remaining.load(std::memory_order_acquire) > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(100));

    const uint64_t t1 = NowNs();

    for (auto* c : coops) c->Shutdown();
    for (int i = 0; i < M; i++) { delete threads[i]; delete coops[i]; }

    const double ms = (t1 - t0) / 1e6;
    const uint64_t units = (uint64_t)totalCtxs * iters;
    const double perUnitNs = (double)(t1 - t0) / (double)units;
    const char* mode = pure ? "pure" : (g_direct ? "direct" : "loop");
    printf("mode=%-6s coops=%d ctxs/coop=%u iters=%llu work=%lluns | "
           "makespan=%.2fms units=%llu per-unit=%.1fns thru=%.1fM/s\n",
           mode, M, g_ctxs, (unsigned long long)iters, (unsigned long long)work,
           ms, (unsigned long long)units, perUnitNs, units / ms / 1e3);
    return 0;
}
