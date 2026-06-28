// Timer fan-out microbenchmark for docs/timer_wheel_001.md.
//
// Spawns N contexts that each sleep for a jittered interval, repeated over R rounds, with the
// deadlines spread across a window wide enough that the cooperator genuinely idles between expiries
// -- the sparse regime where each in-flight sleep costs a kernel timer. Run it under both timer
// modes and compare:
//
//   - makespan (wall time for the whole fan-out),
//   - the cooperator thread's voluntary context switches (RUSAGE_THREAD nvcsw) -- the count of
//     kernel wakeups, the thing the userspace queue collapses,
//   - total sleeps issued (= the number of IORING_OP_TIMEOUT SQEs the kernel-per-timer mode arms,
//     one hrtimer each; the userspace queue arms ~one timer per idle service instead).
//
// Wrap it in `perf stat -e timer:hrtimer_start` to read the kernel hrtimer-arm count directly.
//
// A second workload, `cancel`, models the high-count / low-hit-frequency regime: N sleeps armed with
// a far deadline, then all killed before any fires. Under kernel-per-timer each one still arms (and
// then cancels) a real kernel hrtimer; under the userspace queue a never-fired sleep is a pure
// userspace insert + remove, and only the single nearest timer ever reaches the kernel. This is the
// regime where the queue's kernel hrtimer count collapses from O(N) to O(1).
//
// Usage: bench_timer_fanout <kernel|queue> [fire|cancel] [N] [rounds] [windowUs] [baseUs]

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/resource.h>
#include <thread>
#include <vector>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"

#include <memory>

using namespace coop;

// Pipeline workload: a context per pipeline, each looping [tiny compute -> Sleep], many pipelines
// per cooperator, across several cooperators OS-scheduled on separate cores. This reproduces the
// high-swappiness IO-fanout profile that motivated the design, where the kernel hrtimer machinery
// (lapic_next_deadline / timerqueue_add / __hrtimer_*) showed up as the largest non-compute cost.
// perf record this binary to read the hrtimer symbols' share of cycles in each mode.
//
static double RunPipeline(bool useQueue, int cores, int pipelines, int iters, int sleepUs,
                          int computeIters)
{
    std::vector<std::unique_ptr<Cooperator>> coops;
    std::vector<std::unique_ptr<Thread>> threads;

    auto wall0 = std::chrono::steady_clock::now();

    for (int m = 0; m < cores; m++)
    {
        CooperatorConfiguration cfg;
        cfg.timerMode = useQueue ? TimerMode::UserspaceQueue : TimerMode::KernelPerTimer;
        cfg.cpuAffinity = 1 + m;                 // cores 1..cores
        cfg.SetName("pipeline");
        coops.push_back(std::make_unique<Cooperator>(cfg));
    }
    for (int m = 0; m < cores; m++)
    {
        threads.push_back(std::make_unique<Thread>(coops[m].get()));
    }

    for (int m = 0; m < cores; m++)
    {
        coops[m]->Submit([pipelines, iters, sleepUs, computeIters](Context* drv)
        {
            int remaining = pipelines;
            Coordinator allDone;
            allDone.TryAcquire(drv);

            for (int p = 0; p < pipelines; p++)
            {
                drv->GetCooperator()->Spawn(
                    [&remaining, &allDone, iters, sleepUs, computeIters](Context* pipe)
                {
                    uint64_t acc = 0;
                    for (int it = 0; it < iters; it++)
                    {
                        for (int c = 0; c < computeIters; c++) acc += c * 2654435761u;  // tiny compute
                        static volatile uint64_t sink;
                        sink = acc;                                   // keep the compute from vanishing
                        time::Sleep(pipe, std::chrono::microseconds(sleepUs));
                    }
                    if (--remaining == 0)
                    {
                        allDone.Release(pipe);
                    }
                });
            }
            CoordinateWith(drv, &allDone);
            drv->GetCooperator()->Shutdown();
        });
    }

    threads.clear();                             // join all (Thread destructor)
    auto wall1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(wall1 - wall0).count();
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr,
            "usage: %s <kernel|queue> [fire|cancel|pipeline] [N] [rounds] [windowUs] [baseUs]\n",
            argv[0]);
        return 2;
    }

    if (argc > 2 && strcmp(argv[2], "pipeline") == 0)
    {
        const bool useQueue     = strcmp(argv[1], "queue") == 0;
        const int  cores        = argc > 3 ? atoi(argv[3]) : 3;
        const int  pipelines    = argc > 4 ? atoi(argv[4]) : 500;
        const int  iters        = argc > 5 ? atoi(argv[5]) : 400;
        const int  sleepUs      = argc > 6 ? atoi(argv[6]) : 200;
        const int  computeIters = argc > 7 ? atoi(argv[7]) : 200;
        double ms = RunPipeline(useQueue, cores, pipelines, iters, sleepUs, computeIters);
        printf("mode=%-6s workload=pipeline cores=%d pipelines/core=%d iters=%d sleepUs=%d  ->  "
               "makespan=%.1f ms  (sleeps=%lld)\n",
               useQueue ? "queue" : "kernel", cores, pipelines, iters, sleepUs, ms,
               (long long)cores * pipelines * iters);
        return 0;
    }

    const bool useQueue   = strcmp(argv[1], "queue") == 0;
    const bool cancelMode = argc > 2 && strcmp(argv[2], "cancel") == 0;
    const int  N        = argc > 3 ? atoi(argv[3]) : 500;
    const int  rounds   = argc > 4 ? atoi(argv[4]) : 200;
    const int  windowUs = argc > 5 ? atoi(argv[5]) : 20000;   // jitter spread (fire workload)
    const int  baseUs   = argc > 6 ? atoi(argv[6]) : 1000;    // floor

    CooperatorConfiguration cfg;
    cfg.timerMode = useQueue ? TimerMode::UserspaceQueue : TimerMode::KernelPerTimer;
    cfg.SetName("timer_fanout");

    Cooperator co(cfg);
    Thread t(&co);

    std::atomic<bool> done{false};
    double makespanMs = 0;
    long nvcsw = 0, nivcsw = 0;
    uint64_t totalSleeps = 0;

    co.Submit([&](Context* ctx)
    {
        std::mt19937 rng(0xC007);
        std::uniform_int_distribution<int> jitter(0, windowUs);

        struct rusage ru0;
        getrusage(RUSAGE_THREAD, &ru0);
        auto wall0 = std::chrono::steady_clock::now();

        std::vector<Context::Handle> handles(N);

        for (int r = 0; r < rounds; r++)
        {
            // Block the driver on a coordinator the last finishing child releases, rather than
            // spinning on Yield. Spinning would keep the cooperator perpetually runnable, so it
            // would never idle into a kernel wait and the sparse-timer regime would never arise.
            //
            int remaining = N;
            Coordinator roundDone;
            roundDone.TryAcquire(ctx);

            for (int i = 0; i < N; i++)
            {
                // fire: jittered short deadline that actually elapses. cancel: a far deadline that
                // will be killed before it fires.
                //
                int sleepUs = cancelMode ? 60'000'000 : baseUs + jitter(rng);
                ctx->GetCooperator()->Spawn([&remaining, &roundDone, sleepUs](Context* child)
                {
                    time::Sleep(child, std::chrono::microseconds(sleepUs));
                    // Single-threaded cooperator: plain decrement, no atomics. The last one wakes
                    // the driver.
                    //
                    if (--remaining == 0)
                    {
                        roundDone.Release(child);
                    }
                }, &handles[i]);
            }
            totalSleeps += N;

            if (cancelMode)
            {
                // Let every child register its deadline and block, then kill them all before any
                // fires. Each kill cancels the timer: an io_uring async-cancel of a real hrtimer in
                // kernel mode, a userspace node removal in queue mode.
                //
                ctx->Yield(true);
                for (int i = 0; i < N; i++)
                {
                    handles[i].Kill();
                }
            }

            CoordinateWith(ctx, &roundDone);
        }

        auto wall1 = std::chrono::steady_clock::now();
        struct rusage ru1;
        getrusage(RUSAGE_THREAD, &ru1);

        makespanMs = std::chrono::duration<double, std::milli>(wall1 - wall0).count();
        nvcsw  = ru1.ru_nvcsw  - ru0.ru_nvcsw;
        nivcsw = ru1.ru_nivcsw - ru0.ru_nivcsw;

        done.store(true, std::memory_order_release);
        ctx->GetCooperator()->Shutdown();
    });

    while (!done.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    printf("mode=%-6s workload=%-6s N=%d rounds=%d  ->  makespan=%.1f ms  "
           "nvcsw=%ld nivcsw=%ld  sleeps=%lu\n",
           useQueue ? "queue" : "kernel", cancelMode ? "cancel" : "fire", N, rounds,
           makespanMs, nvcsw, nivcsw, (unsigned long)totalSleeps);
    return 0;
}
