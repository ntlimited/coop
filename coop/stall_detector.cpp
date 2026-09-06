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
#include "detail/stack_walk.h"
#include "detail/stall_capture.h"
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

// A watchdog serializes requests and report copying; the generation-tagged mailbox also protects
// against handlers that arrive or finish after that watchdog's bounded wait has expired.
//
detail::StallCapture g_capture;
std::mutex          g_captureMutex;

std::mutex      g_installMutex;
int             g_handlerRefs = 0;
struct sigaction g_prevAction;

void StallSignalHandler(int, siginfo_t* info, void* uctx)
{
    if (!g_capture.Claim(info)) return;
    auto cookie = reinterpret_cast<detail::StallCapture::Cookie>(info->si_value.sival_ptr);
    Cooperator* co  = Cooperator::thread_cooperator;
    // A delayed signal must not attribute a recovered stall to whichever context runs next.
    // Read context data only here, while the interrupted cooperator cannot destroy it.
    //
    if (!co || co->StallState() != g_capture.stallState)
    {
        g_capture.context = nullptr;
        g_capture.name[0] = '\0';
        g_capture.depth = 0;
        g_capture.Complete(cookie);
        return;
    }
    Context* ctx = co->Scheduled();

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

    // Bounded frame-pointer walk from the interrupted machine context -- no locks or allocation.
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

    detail::StackBounds bounds{0, 0};
    if (ctx)
    {
        bounds = {reinterpret_cast<uintptr_t>(ctx->m_segment.Bottom()),
                  reinterpret_cast<uintptr_t>(ctx->m_segment.Top())};
    }
    g_capture.depth = detail::WalkInterruptedStack(pc, fp, sp, bounds,
                                                  g_capture.frames, StallDetector::kMaxFrames);

    g_capture.Complete(cookie);
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
            auto cookie = g_capture.Begin(state);
            if (cookie && detail::QueueStallSignal(tid, cookie))
            {
                // Wait briefly for the handler to finish. On timeout, cancel an unclaimed request;
                // a handler already capturing retains the slot until it finishes. Neither case
                // permits a later watchdog to read or overwrite an in-progress capture.
                //
                for (int i = 0; i < 500 && !g_capture.Ready(cookie); ++i)
                {
                    struct timespec ts{0, 100000};   // 100us
                    nanosleep(&ts, nullptr);
                }
                if (g_capture.Ready(cookie))
                {
                    rep.context = g_capture.context;
                    rep.depth   = g_capture.depth;
                    memcpy(rep.name, g_capture.name, sizeof(rep.name));
                    for (int i = 0; i < g_capture.depth && i < kMaxFrames; ++i)
                        rep.frames[i] = g_capture.frames[i];
                }
            }
            if (cookie) g_capture.Cancel(cookie);
        }

        m_stallCount.fetch_add(1, std::memory_order_relaxed);
        if (m_hook) m_hook(rep);
    }
}

} // end namespace coop
