#pragma once

#include <atomic>
#include <cstdint>
#include <csignal>
#include <limits>
#include <sys/syscall.h>
#include <unistd.h>

#include "../stall_detector.h"

namespace coop::detail
{

// A watchdog timeout does not mean its signal handler has finished, or even started. Keep the
// payload alive process-wide and give each request a cookie: only the matching handler may claim
// it, and a claimed payload cannot be reused until that handler publishes completion.
//
// Watchdog methods (Begin, Cancel, Ready and payload reads) require external serialization.
// Claim and Complete run in the signal handler and must remain lock-free. A late completion is
// discarded by the next Begin; a handler stuck mid-capture makes subsequent captures unavailable,
// without forcing watchdogs or detector destruction to wait indefinitely.
//
class StallCapture
{
public:
    using Cookie = uintptr_t;
    static_assert(sizeof(Cookie) == sizeof(uint64_t));
    static_assert(std::atomic<Cookie>::is_always_lock_free);

    Cookie Begin(uint64_t expectedStallState)
    {
        Cookie phase = m_state.load(std::memory_order_acquire) & kPhaseMask;
        if (phase == kRequested || phase == kCapturing) return 0;

        // Never reuse a cookie, even at wraparound: an arbitrarily old signal can still be pending.
        //
        if (m_generation == std::numeric_limits<Cookie>::max() >> 2) return 0;
        Cookie cookie = (++m_generation << 2) | kRequested;
        stallState = expectedStallState;
        m_state.store(cookie, std::memory_order_release);
        return cookie;
    }

    bool Claim(const siginfo_t* info)
    {
        if (!info || info->si_code != SI_QUEUE || info->si_pid != getpid()) return false;
        Cookie cookie = reinterpret_cast<Cookie>(info->si_value.sival_ptr);
        if ((cookie & kPhaseMask) != kRequested) return false;
        return m_state.compare_exchange_strong(cookie, cookie + 1, std::memory_order_acquire,
                                               std::memory_order_relaxed);
    }

    void Complete(Cookie cookie)
    {
        m_state.store(cookie + 2, std::memory_order_release);
    }

    bool Ready(Cookie cookie) const
    {
        return m_state.load(std::memory_order_acquire) == cookie + 2;
    }

    void Cancel(Cookie cookie)
    {
        m_state.compare_exchange_strong(cookie, cookie & ~kPhaseMask, std::memory_order_acq_rel,
                                       std::memory_order_relaxed);
    }

    uint64_t  stallState{0};
    Context*  context{nullptr};
    char      name[64]{};
    uintptr_t frames[StallDetector::kMaxFrames]{};
    int       depth{0};

private:
    static constexpr Cookie kPhaseMask = 3;
    static constexpr Cookie kRequested = 1;
    static constexpr Cookie kCapturing = 2;

    std::atomic<Cookie> m_state{0};
    Cookie             m_generation{0};
};

inline bool QueueStallSignal(int tid, StallCapture::Cookie cookie)
{
    siginfo_t info{};
    info.si_signo = SIGURG;
    info.si_code = SI_QUEUE;
    info.si_pid = getpid();
    info.si_uid = getuid();
    info.si_value.sival_ptr = reinterpret_cast<void*>(cookie);
    return syscall(SYS_rt_tgsigqueueinfo, getpid(), tid, SIGURG, &info) == 0;
}

} // namespace coop::detail
