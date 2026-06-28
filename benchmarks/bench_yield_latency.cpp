// directYield IO-latency probe -- the *benefit* half of the directYield tradeoff (the throughput
// half is bench_work_yield).
//
// A probe context loops [Sleep(d) -> record how late the wake was]; N spinner contexts loop
// [Work -> Yield], continuously runnable so the cooperator stays in the fastpath. The probe's wake
// lateness over its intended deadline is how long the timer completion waited to be polled:
//   - directYield OFF: the loop polls every resume, so the wake is prompt (kernel timer slack only).
//   - directYield ON, governor disabled (ioPresentLimit=0): the timer CQE waits up to
//     directYieldBudget spinner-switches before a poll.
//   - directYield ON, governor on: it waits ~ioPresentLimit switches.
//
// Spinner work is deliberately coarse (microseconds) so switches-to-poll converts to a visible
// wall-clock lateness well above the kernel timer slot.
//
// Usage: bench_yield_latency [--ctxs=8] [--work=10000] [--direct] [--budget=64] [--iopresent=8]
//                            [--probe-iters=400] [--probe-sleep=300] [--nopin]
//
#include <algorithm>
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
#include "coop/time/interval.h"
#include "coop/time/sleep.h"

using Clock = std::chrono::steady_clock;

static inline uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}
static inline void BusyFor(uint64_t ns)
{
    if (!ns) return;
    const uint64_t start = NowNs();
    volatile uint64_t sink = 0;
    while (NowNs() - start < ns)
        for (int i = 0; i < 64; i++) sink = sink + (uint64_t)i * 2654435761u;
    (void)sink;
}

static uint32_t g_ctxs = 8, g_budget = 64;
static int g_iopresent = 8;
static uint64_t g_work = 10000;        // spinner work ns
static int g_probeIters = 400;
static uint64_t g_probeSleepUs = 300;  // probe sleep interval
static bool g_direct = false;
static bool g_noTaskrun = false;

int main(int argc, char** argv)
{
    bool pin = true;
    for (int i = 1; i < argc; i++)
    {
        auto eq = [&](const char* k){ return strncmp(argv[i], k, strlen(k)) == 0; };
        if (eq("--ctxs=")) g_ctxs = atoi(argv[i] + 7);
        else if (eq("--work=")) g_work = strtoull(argv[i] + 7, nullptr, 10);
        else if (eq("--budget=")) g_budget = atoi(argv[i] + 9);
        else if (eq("--iopresent=")) g_iopresent = atoi(argv[i] + 12);
        else if (eq("--probe-iters=")) g_probeIters = atoi(argv[i] + 14);
        else if (eq("--probe-sleep=")) g_probeSleepUs = strtoull(argv[i] + 14, nullptr, 10);
        else if (eq("--direct")) g_direct = true;
        else if (eq("--notaskrun")) g_noTaskrun = true;
        else if (eq("--nopin")) pin = false;
    }

    coop::CooperatorConfiguration cfg;
    cfg.directYield = g_direct;
    cfg.directYieldBudget = (int)g_budget;
    cfg.ioPresentLimit = g_iopresent;
    if (g_noTaskrun) cfg.uring.coopTaskrun = false;

    coop::Cooperator co(cfg);
    coop::Thread th(&co);
    if (pin) th.PinToCore(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    const uint32_t ctxs = g_ctxs;
    const uint64_t work = g_work;
    const int probeIters = g_probeIters;
    const uint64_t probeSleepNs = g_probeSleepUs * 1000ull;
    const auto d = std::chrono::duration_cast<coop::time::Interval>(
        std::chrono::microseconds(g_probeSleepUs));

    co.SubmitSync([&](coop::Context* ctx)
    {
        bool done = false;
        std::vector<uint64_t> lat;
        lat.reserve(probeIters);

        // N spinners hammering the fastpath, continuously runnable until the probe is finished.
        //
        for (uint32_t k = 0; k < ctxs; k++)
            coop::Spawn([&done, work](coop::Context* c)
            {
                c->Detach();
                while (!done) { BusyFor(work); c->Yield(true); }
            });

        ctx->Yield(true);  // let the spinners reach their loop

        // Probe: sleep, then measure how far past the deadline the wake actually landed.
        //
        for (int i = 0; i < probeIters; i++)
        {
            const uint64_t t0 = NowNs();
            coop::time::Sleep(ctx, d);
            const uint64_t woke = NowNs();
            const uint64_t deadline = t0 + probeSleepNs;
            lat.push_back(woke > deadline ? woke - deadline : 0);
        }

        done = true;
        for (uint32_t i = 0; i < ctxs * 8; i++) ctx->Yield(true);  // let spinners observe done + exit

        std::sort(lat.begin(), lat.end());
        auto pct = [&](double p){ return lat[(size_t)(p * (lat.size() - 1))] / 1000.0; };
        double sum = 0; for (auto v : lat) sum += v;
        const char* mode = !g_direct ? "off" : (g_iopresent == 0 ? "count-only" : "governor");
        printf("mode=%-10s ctxs=%u work=%lluns budget=%u iopresent=%d | "
               "wake lateness us: mean=%.1f p50=%.1f p99=%.1f max=%.1f (n=%zu)\n",
               mode, ctxs, (unsigned long long)work, g_budget, g_iopresent,
               sum / lat.size() / 1000.0, pct(0.50), pct(0.99), pct(1.0), lat.size());
    });

    co.Shutdown();
    return 0;
}
