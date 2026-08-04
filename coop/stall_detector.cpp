#include "stall_detector.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <spdlog/spdlog.h>

#include "context.h"
#include "cooperator.h"
#include "time/now.h"

namespace coop
{

// The stall capture rides SIGURG: its default disposition is ignore, io_uring does not use it, and
// unlike the profiler's SIGPROF it is not tied to a CPU-time timer -- we deliver it by hand, only to
// the one thread we have caught stalling. The handler runs ON that thread, so it can safely read the
// thread-local running context and walk the stack it is stuck in.
//
namespace
{

struct Capture
{
    std::atomic<bool> ready{false};
    Context*          context{nullptr};
    char              name[64]{};
    uintptr_t         frames[StallDetector::kMaxFrames]{};
    int               depth{0};
};

// One capture in flight process-wide. A watchdog holds g_captureMutex across signal + wait, so the
// handler (which cannot lock) always writes into a slot owned by exactly one waiter.
//
Capture         g_capture;
std::mutex      g_captureMutex;

std::mutex      g_installMutex;
int             g_handlerRefs = 0;
struct sigaction g_prevAction;

void StallSignalHandler(int, siginfo_t*, void* uctx)
{
    Cooperator* co  = Cooperator::thread_cooperator;
    Context*    ctx = co ? co->Scheduled() : nullptr;

    g_capture.context = ctx;

    // Snapshot the name by hand -- strncpy is not on the async-signal-safe list. GetName returns a
    // stable pointer into the context, safe to read here.
    //
    const char* nm = ctx ? ctx->GetName() : nullptr;
    int n = 0;
    if (nm)
    {
        for (; n < 63 && nm[n]; ++n) g_capture.name[n] = nm[n];
    }
    g_capture.name[n] = '\0';

    // Frame-pointer walk from the interrupted machine context -- no calls, no locks, no malloc.
    // Valid because coop is built -fno-omit-frame-pointer; user frames unwind as far as they too
    // keep frame pointers, and the top PC is always recorded regardless.
    //
    auto* u = static_cast<ucontext_t*>(uctx);
#if defined(__x86_64__)
    uintptr_t pc = u->uc_mcontext.gregs[REG_RIP];
    uintptr_t fp = u->uc_mcontext.gregs[REG_RBP];
    uintptr_t sp = u->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
    uintptr_t pc = u->uc_mcontext.pc;
    uintptr_t fp = u->uc_mcontext.regs[29];
    uintptr_t sp = u->uc_mcontext.sp;
#else
#error "Unsupported architecture for StallDetector"
#endif

    int depth = 0;
    g_capture.frames[depth++] = pc;
    while (depth < StallDetector::kMaxFrames && fp > sp && (fp & 7) == 0)
    {
        auto* frame = reinterpret_cast<uintptr_t*>(fp);
        uintptr_t ret = frame[1];
        if (ret == 0) break;
        g_capture.frames[depth++] = ret;
        uintptr_t nextFp = frame[0];
        if (nextFp <= fp) break;
        fp = nextFp;
    }
    g_capture.depth = depth;

    g_capture.ready.store(true, std::memory_order_release);
}

void InstallHandler()
{
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (g_handlerRefs++ == 0)
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = StallSignalHandler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGURG, &sa, &g_prevAction);
    }
}

void RemoveHandler()
{
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (--g_handlerRefs == 0)
    {
        sigaction(SIGURG, &g_prevAction, nullptr);
    }
}

void DefaultReport(const StallDetector::Report& r)
{
    std::string frames;
    frames.reserve(r.depth * 16);
    for (int i = 0; i < r.depth; ++i)
    {
        char buf[20];
        snprintf(buf, sizeof(buf), "%s0x%lx", i ? " " : "", (unsigned long)r.frames[i]);
        frames += buf;
    }
    spdlog::warn("coop stall: context '{}' held the cooperator for {} us without yielding; stack: [{}]",
                 r.name[0] ? r.name : "(unnamed)", r.stalledForUs, frames);
}

} // namespace

StallDetector::StallDetector(Cooperator* co, time::Interval threshold, Hook hook)
    : m_co(co)
    , m_thresholdUs(threshold.count())
    , m_hook(hook ? std::move(hook) : Hook(&DefaultReport))
{
    if (m_thresholdUs < 1) m_thresholdUs = 1;
    InstallHandler();
    m_co->ArmStall(true);
    m_watchdog = std::thread([this] { Watch(); });
}

StallDetector::~StallDetector()
{
    m_stop.store(true, std::memory_order_release);
    if (m_watchdog.joinable()) m_watchdog.join();
    m_co->ArmStall(false);
    RemoveHandler();
}

void StallDetector::Watch()
{
    // Poll fast enough that detection latency is ~threshold, but never busier than 1ms nor slower
    // than 100ms.
    //
    int64_t pollUs = m_thresholdUs / 8;
    if (pollUs < 1000)   pollUs = 1000;
    if (pollUs > 100000) pollUs = 100000;

    std::mutex             m;
    std::condition_variable cv;

    uint64_t lastState   = 0;
    int64_t  stateSince  = time::MonotonicMicros();
    bool     reported    = false;

    while (!m_stop.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::microseconds(pollUs),
                        [this] { return m_stop.load(std::memory_order_acquire); });
        }
        if (m_stop.load(std::memory_order_acquire)) break;

        uint64_t state = m_co->StallState();
        int64_t  now   = time::MonotonicMicros();

        // A change in state -- a switch into or out of a context -- means the cooperator made
        // progress. Reset the clock. Only an unchanging running state is a stall.
        //
        if (state != lastState)
        {
            lastState  = state;
            stateSince = now;
            reported   = false;
            continue;
        }

        bool running = (state & 1u) != 0;
        if (!running || reported) continue;

        if (now - stateSince < m_thresholdUs) continue;

        // Genuine stall: one context has been running, unchanged, past the threshold. Deliver the
        // capture signal to the running thread and collect the stack it is stuck in.
        //
        reported = true;

        Report rep;
        memset(&rep, 0, sizeof(rep));
        rep.stalledForUs = now - stateSince;

        int tid = m_co->Tid();
        if (tid != 0)
        {
            std::lock_guard<std::mutex> cap(g_captureMutex);
            g_capture.ready.store(false, std::memory_order_release);
            g_capture.depth = 0;

            if (syscall(SYS_tgkill, getpid(), tid, SIGURG) == 0)
            {
                // Wait briefly for the handler to land. If the thread is wedged uninterruptibly or
                // has since exited, proceed with whatever (possibly nothing) was captured.
                //
                for (int i = 0; i < 500 && !g_capture.ready.load(std::memory_order_acquire); ++i)
                {
                    struct timespec ts{0, 100000};   // 100us
                    nanosleep(&ts, nullptr);
                }
                if (g_capture.ready.load(std::memory_order_acquire))
                {
                    rep.context = g_capture.context;
                    rep.depth   = g_capture.depth;
                    memcpy(rep.name, g_capture.name, sizeof(rep.name));
                    for (int i = 0; i < g_capture.depth && i < kMaxFrames; ++i)
                        rep.frames[i] = g_capture.frames[i];
                }
            }
        }

        m_stallCount.fetch_add(1, std::memory_order_relaxed);
        if (m_hook) m_hook(rep);
    }
}

} // end namespace coop
