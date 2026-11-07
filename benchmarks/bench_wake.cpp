// Standalone targeted-wake panel. See docs/tiered_wake_01.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

#include "coop/cooperator.h"
#include "coop/thread.h"

using Clock = std::chrono::steady_clock;
using coop::detail::WakeTier;

namespace
{
struct Worker
{
    coop::Cooperator co;
    coop::Thread thread;
    explicit Worker(coop::CooperatorConfiguration const& config) : co(config), thread(&co) {}
    ~Worker() { co.Shutdown(); }
};

long Micros(timeval t)
{
    return t.tv_sec * 1000000L + t.tv_usec;
}

void Panel(std::string const& mode, int n, bool warm, int samples)
{
    auto config = coop::s_defaultCooperatorConfiguration;
    config.tieredWake = mode != "eventfd";
    config.wakeDiagnostics = true;
    config.wakeHotUs = mode == "hot" ? 100 : 0;
    config.wakeSemiColdUs = mode == "semi" ? 100 : 0;
    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<bool> stopWarm{false};
    for (int i = 0; i < n; ++i)
    {
        workers.emplace_back(std::make_unique<Worker>(config));
        if (warm)
            workers.back()->co.Submit(
                [&](coop::Context* ctx)
                {
                    while (!stopWarm.load() && !ctx->IsKilled())
                        ctx->Yield(true);
                });
    }
    WakeTier desired = mode == "cold"   ? WakeTier::Cold
                       : mode == "semi" ? WakeTier::SemiCold
                                        : WakeTier::Hot;
    std::vector<double> latency;
    std::vector<double> tierLatency[3];
    latency.reserve(samples);
    uint64_t counts[3]{};
    int missed = 0;
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    for (int i = 0; i < samples; ++i)
    {
        auto& co = workers[i % n]->co;
        if (!warm)
        {
            if (mode == "eventfd")
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            else
            {
                if (!co.SubmitSync([](coop::Context*) {}))
                    std::abort();
                auto end = Clock::now() + std::chrono::milliseconds(10);
                while (co.WakeState() != desired && Clock::now() < end)
                    coop::detail::WakePause();
                if (co.WakeState() != desired)
                    ++missed;
            }
        }
        uint64_t prior[3];
        for (int t = 0; t < 3; ++t)
            prior[t] = co.WakeCount(WakeTier(t));
        std::atomic<bool> done{false};
        double ns = 0;
        auto start = Clock::now();
        if (!co.Submit(
                [&](coop::Context*)
                {
                    ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
                    done.store(true, std::memory_order_release);
                }))
            std::abort();
        while (!done.load(std::memory_order_acquire))
            coop::detail::WakePause();
        latency.push_back(ns);
        for (int t = 0; t < 3; ++t)
        {
            auto delta = co.WakeCount(WakeTier(t)) - prior[t];
            counts[t] += delta;
            if (delta == 1)
                tierLatency[t].push_back(ns);
        }
    }
    std::sort(latency.begin(), latency.end());

    // Batch submission measures queueing plus context spawn/drain, without per-task handshakes.
    constexpr int tasks = 32768;
    std::atomic<int> completed{0};
    auto start = Clock::now();
    for (int i = 0; i < tasks; ++i)
        if (!workers[i % n]->co.Submit([&](coop::Context*) { completed.fetch_add(1); }))
            std::abort();
    while (completed.load() != tasks)
        coop::detail::WakePause();
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    getrusage(RUSAGE_SELF, &after);
    stopWarm.store(true);
    auto monitor = coop::detail::MonitorName(workers.front()->co.WakeMonitor());
    cpu_set_t mask;
    CPU_ZERO(&mask);
    sched_getaffinity(0, sizeof(mask), &mask);
    std::string tierMedians = "[";
    for (int t = 0; t < 3; ++t)
    {
        auto& values = tierLatency[t];
        std::sort(values.begin(), values.end());
        if (t)
            tierMedians += ",";
        tierMedians += values.empty() ? "null" : std::to_string(values[values.size() / 2]);
    }
    tierMedians += "]";
    std::printf(
        "{\"resource_scope\":\"latency-preparation-and-throughput\",\"tier_p50_ns\":%s,\"mode\":\"%s\",\"cooperators\":%d,\"warm\":%s,\"monitor\":\"%s\",\"cold_transport\":\"%s\","
        "\"cpus\":%d,\"samples\":%d,\"p50_ns\":%.1f,\"p95_ns\":%.1f,\"p99_ns\":%.1f,"
        "\"tasks_per_s\":%.1f,\"observed_hot\":%llu,\"observed_semi\":%llu,"
        "\"observed_cold\":%llu,\"missed_tier\":%d,\"user_us\":%ld,\"system_us\":%ld,"
        "\"minor_faults\":%ld,\"major_faults\":%ld,\"voluntary_switches\":%ld,"
        "\"involuntary_switches\":%ld}\n",
        tierMedians.c_str(), mode.c_str(), n, warm ? "true" : "false", monitor,
        workers.front()->co.WakeUsesMsgRing() ? "msg_ring" : "eventfd", CPU_COUNT(&mask), samples,
        latency[samples / 2], latency[samples * 95 / 100], latency[samples * 99 / 100],
        tasks / seconds, (unsigned long long)counts[0], (unsigned long long)counts[1],
        (unsigned long long)counts[2], missed, Micros(after.ru_utime) - Micros(before.ru_utime),
        Micros(after.ru_stime) - Micros(before.ru_stime), after.ru_minflt - before.ru_minflt,
        after.ru_majflt - before.ru_majflt, after.ru_nvcsw - before.ru_nvcsw,
        after.ru_nivcsw - before.ru_nivcsw);
    std::fflush(stdout);
    workers.clear(); // Join warm contexts before stopWarm and captured counters die.
}
} // namespace

int main(int argc, char** argv)
{
    std::string onlyMode;
    int onlyN = 0, samples = 1000;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc)
            onlyMode = argv[++i];
        else if (arg == "--cooperators" && i + 1 < argc)
            onlyN = std::atoi(argv[++i]);
        else if (arg == "--samples" && i + 1 < argc)
            samples = std::atoi(argv[++i]);
        else
            return 2;
    }
    if (samples < 100 || samples > 1000000 || onlyN < 0 || onlyN > 16)
        return 2;
    if (!onlyMode.empty() && onlyMode != "eventfd" && onlyMode != "hot" && onlyMode != "semi" &&
        onlyMode != "cold")
        return 2;
    for (int n = onlyN ? onlyN : 1; n <= (onlyN ? onlyN : 16); ++n)
        for (auto const& mode : {"eventfd", "hot", "semi", "cold"})
        {
            if (!onlyMode.empty() && onlyMode != mode)
                continue;
            for (bool warm : {false, true})
                Panel(mode, n, warm, samples);
        }
}
