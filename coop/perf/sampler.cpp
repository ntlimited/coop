#include "sampler.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <sys/time.h>
#include <ucontext.h>

#include "coop/cooperator.h"
#include "coop/context.h"

namespace coop
{
namespace perf
{

// Ring buffer for samples. Power-of-2 capacity for fast masking.
//
static constexpr size_t RING_CAPACITY = 8192;
static constexpr size_t RING_MASK = RING_CAPACITY - 1;

static Sample              g_ring[RING_CAPACITY];
static std::atomic<size_t> g_head{0};       // next write index (signal handler)
static std::atomic<size_t> g_total{0};      // total samples captured
static std::atomic<int>    g_hz{0};         // 0 = not sampling
static struct sigaction    g_prevAction;     // saved previous SIGPROF handler

static inline uint64_t rdtsc()
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// SIGPROF handler — must be async-signal-safe.
//
// Reads the interrupted instruction pointer from the signal context and the current coop
// context from the thread-local cooperator. Writes a sample to the ring buffer.
//
static void SigprofHandler(int, siginfo_t*, void* uctx)
{
    auto* u = static_cast<ucontext_t*>(uctx);
    uintptr_t pc = u->uc_mcontext.gregs[REG_RIP];

    Context* ctx = nullptr;
    Cooperator* co = Cooperator::thread_cooperator;
    if (co)
    {
        ctx = co->Scheduled();
    }

    size_t idx = g_head.fetch_add(1, std::memory_order_relaxed) & RING_MASK;
    g_ring[idx] = {pc, ctx, co, rdtsc()};
    g_total.fetch_add(1, std::memory_order_relaxed);

    // Per-context sample count — safe because signal fires on the same thread as the context.
    //
    if (ctx)
    {
        ++ctx->m_statistics.samples;
    }
}

bool StartSampling(int hz /* = 99 */)
{
    if (hz <= 0) return false;

    // Install SIGPROF handler
    //
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = SigprofHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGPROF, &sa, &g_prevAction) != 0)
    {
        return false;
    }

    // Arm ITIMER_PROF — counts CPU time consumed by the process.
    //
    struct itimerval timer;
    int usec = 1000000 / hz;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = usec;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = usec;

    if (setitimer(ITIMER_PROF, &timer, nullptr) != 0)
    {
        sigaction(SIGPROF, &g_prevAction, nullptr);
        return false;
    }

    g_hz.store(hz, std::memory_order_relaxed);
    return true;
}

void StopSampling()
{
    // Disarm timer
    //
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_PROF, &timer, nullptr);

    // Restore previous handler
    //
    sigaction(SIGPROF, &g_prevAction, nullptr);
    g_hz.store(0, std::memory_order_relaxed);
}

bool IsSampling()
{
    return g_hz.load(std::memory_order_relaxed) != 0;
}

int SamplingHz()
{
    return g_hz.load(std::memory_order_relaxed);
}

size_t ReadSamples(Sample* out, size_t maxSamples)
{
    size_t head = g_head.load(std::memory_order_acquire);
    size_t total = g_total.load(std::memory_order_relaxed);

    // How many samples are in the ring (capped at capacity)
    //
    size_t available = total < RING_CAPACITY ? total : RING_CAPACITY;
    size_t count = available < maxSamples ? available : maxSamples;
    if (count == 0) return 0;

    // Read oldest-first: start from (head - available), walk forward
    //
    size_t start = head - available;
    for (size_t i = 0; i < count; i++)
    {
        out[i] = g_ring[(start + i) & RING_MASK];
    }

    return count;
}

void ResetSamples()
{
    g_head.store(0, std::memory_order_relaxed);
    g_total.store(0, std::memory_order_relaxed);
}

size_t TotalSamples()
{
    return g_total.load(std::memory_order_relaxed);
}

size_t SampleCapacity()
{
    return RING_CAPACITY;
}

} // end namespace coop::perf
} // end namespace coop
