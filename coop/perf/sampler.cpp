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

// PC-only ring buffer. Power-of-2 capacity for fast masking.
//
static constexpr size_t RING_CAPACITY = 8192;
static constexpr size_t RING_MASK = RING_CAPACITY - 1;

static Sample              g_ring[RING_CAPACITY];
static std::atomic<size_t> g_head{0};
static std::atomic<size_t> g_total{0};

// Stack trace ring buffer. Smaller capacity due to larger entry size.
//
static constexpr size_t STACK_RING_CAPACITY = 2048;
static constexpr size_t STACK_RING_MASK = STACK_RING_CAPACITY - 1;

static StackSample              g_stackRing[STACK_RING_CAPACITY];
static std::atomic<size_t>      g_stackHead{0};
static std::atomic<size_t>      g_stackTotal{0};

static std::atomic<int>    g_hz{0};
static std::atomic<bool>   g_stackMode{false};
static std::atomic<int>    g_stackEvery{10};  // backtrace every Nth signal (others get RIP only)
static std::atomic<size_t> g_signalOrdinal{0};
static struct sigaction    g_prevAction;

static inline uint64_t rdtsc()
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
#error "Unsupported architecture for rdtsc"
#endif
}

// SIGPROF handler — must be async-signal-safe.
//
// backtrace() is async-signal-safe on glibc/Linux. It uses DWARF .eh_frame unwind info,
// so it works even without frame pointers (-fomit-frame-pointer, the default at -O2).
//
static void SigprofHandler(int, siginfo_t*, void* uctx)
{
    Context* ctx = nullptr;
    Cooperator* co = Cooperator::thread_cooperator;
    if (co)
    {
        ctx = co->Scheduled();
    }

    auto* u = static_cast<ucontext_t*>(uctx);
#if defined(__x86_64__)
    uintptr_t pc = u->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    uintptr_t pc = u->uc_mcontext.pc;
#endif
    uint64_t ts = rdtsc();

    // Always record RIP — cheap (one register read + atomic bump).
    //
    size_t pcIdx = g_head.fetch_add(1, std::memory_order_relaxed) & RING_MASK;
    g_ring[pcIdx] = {pc, ctx, co, ts};
    g_total.fetch_add(1, std::memory_order_relaxed);

    // In stack mode, subsample backtrace() every Nth signal. The DWARF unwinder is expensive
    // and pollutes the icache/dcache, so we limit its frequency while keeping full-rate PC data.
    //
    if (g_stackMode.load(std::memory_order_relaxed))
    {
        size_t ord = g_signalOrdinal.fetch_add(1, std::memory_order_relaxed);
        int every = g_stackEvery.load(std::memory_order_relaxed);

        bool doStack = (every > 0) && ((ord % every) == 0);

        // Skip backtrace() during shutdown — context stacks may be partially torn down
        // and the unwinder can crash on asm trampolines without .eh_frame info.
        //
        if (doStack && co && co->IsShuttingDown())
        {
            doStack = false;
        }

        if (doStack)
        {
            size_t idx = g_stackHead.fetch_add(1, std::memory_order_relaxed) & STACK_RING_MASK;
            auto& s = g_stackRing[idx];

            // Manual frame pointer walk — no function calls, no locks, no malloc.
            // Requires -fno-omit-frame-pointer. Just a few memory loads per frame.
            //
            s.frames[0] = pc;
            int depth = 1;

#if defined(__x86_64__)
            uintptr_t fp = u->uc_mcontext.gregs[REG_RBP];
            uintptr_t sp = u->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
            uintptr_t fp = u->uc_mcontext.regs[29];    // x29 = frame pointer
            uintptr_t sp = u->uc_mcontext.sp;
#endif

            while (depth < MAX_STACK_DEPTH && fp > sp && (fp & 7) == 0)
            {
                auto* frame = reinterpret_cast<uintptr_t*>(fp);
                uintptr_t retAddr = frame[1];
                if (retAddr == 0) break;
                s.frames[depth++] = retAddr;

                uintptr_t nextFp = frame[0];
                if (nextFp <= fp) break; // must increase (unwinding toward stack base)
                fp = nextFp;
            }

            s.depth = static_cast<uint8_t>(depth);
            s.context = ctx;
            s.cooperator = co;
            s.timestamp = ts;
            g_stackTotal.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (ctx)
    {
        ++ctx->m_statistics.samples;
    }
}

bool StartSampling(int hz /* = 99 */, bool stacks /* = false */)
{
    if (hz <= 0) return false;

    g_stackMode.store(stacks, std::memory_order_relaxed);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = SigprofHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGPROF, &sa, &g_prevAction) != 0)
    {
        return false;
    }

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
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_PROF, &timer, nullptr);

    sigaction(SIGPROF, &g_prevAction, nullptr);
    g_hz.store(0, std::memory_order_relaxed);
}

bool IsSampling()
{
    return g_hz.load(std::memory_order_relaxed) != 0;
}

bool IsStackMode()
{
    return g_stackMode.load(std::memory_order_relaxed);
}

int SamplingHz()
{
    return g_hz.load(std::memory_order_relaxed);
}

size_t ReadSamples(Sample* out, size_t maxSamples)
{
    size_t head = g_head.load(std::memory_order_acquire);
    size_t total = g_total.load(std::memory_order_relaxed);

    size_t available = total < RING_CAPACITY ? total : RING_CAPACITY;
    size_t count = available < maxSamples ? available : maxSamples;
    if (count == 0) return 0;

    size_t start = head - available;
    for (size_t i = 0; i < count; i++)
    {
        out[i] = g_ring[(start + i) & RING_MASK];
    }
    return count;
}

size_t ReadStackSamples(StackSample* out, size_t maxSamples)
{
    size_t head = g_stackHead.load(std::memory_order_acquire);
    size_t total = g_stackTotal.load(std::memory_order_relaxed);

    size_t available = total < STACK_RING_CAPACITY ? total : STACK_RING_CAPACITY;
    size_t count = available < maxSamples ? available : maxSamples;
    if (count == 0) return 0;

    size_t start = head - available;
    for (size_t i = 0; i < count; i++)
    {
        out[i] = g_stackRing[(start + i) & STACK_RING_MASK];
    }
    return count;
}

void ResetSamples()
{
    g_head.store(0, std::memory_order_relaxed);
    g_total.store(0, std::memory_order_relaxed);
    g_stackHead.store(0, std::memory_order_relaxed);
    g_stackTotal.store(0, std::memory_order_relaxed);
    g_signalOrdinal.store(0, std::memory_order_relaxed);
}

void SetStackSubsample(int every)
{
    if (every < 1) every = 1;
    g_stackEvery.store(every, std::memory_order_relaxed);
}

int StackSubsample()
{
    return g_stackEvery.load(std::memory_order_relaxed);
}

size_t TotalSamples()
{
    if (g_stackMode.load(std::memory_order_relaxed))
        return g_stackTotal.load(std::memory_order_relaxed);
    return g_total.load(std::memory_order_relaxed);
}

size_t SampleCapacity()
{
    if (g_stackMode.load(std::memory_order_relaxed))
        return STACK_RING_CAPACITY;
    return RING_CAPACITY;
}

} // end namespace coop::perf
} // end namespace coop
