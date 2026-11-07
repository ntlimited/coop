#include "doorbell.h"

#if defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace coop::detail
{
MonitorKind DetectMonitor()
{
#if defined(__x86_64__)
    unsigned a, b, c, d;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d) && (c & (1u << 5)))
        return MonitorKind::WaitPkg;
    if (__get_cpuid(0x80000001, &a, &b, &c, &d) && (c & (1u << 29)))
        return MonitorKind::MonitorX;
#endif
    return MonitorKind::Pause;
}

char const* MonitorName(MonitorKind kind)
{
    switch (kind)
    {
        case MonitorKind::WaitPkg:
            return "waitpkg";
        case MonitorKind::MonitorX:
            return "monitorx";
        case MonitorKind::Pause:
            return "pause";
    }
    return "invalid";
}

void WakePause()
{
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

void MonitorWait(Doorbell& bell, uint64_t expected, MonitorKind kind)
{
#if defined(__x86_64__)
    if (kind == MonitorKind::WaitPkg)
    {
        MonitorThenWait(
            bell, expected, [&] { asm volatile("umonitor %0" ::"r"(&bell.word)
                                               : "memory"); },
            [&]
            {
                uint64_t deadline = __rdtsc() + 1000;
                // C0.2 request, bounded/restricted by this deadline and kernel UMWAIT_CONTROL.
                asm volatile("umwait %%ecx" ::"a"(uint32_t(deadline)),
                             "d"(uint32_t(deadline >> 32)), "c"(0)
                             : "cc", "memory");
            });
        return;
    }
    if (kind == MonitorKind::MonitorX)
    {
        MonitorThenWait(
            bell, expected,
            [&] { asm volatile("monitorx" ::"a"(&bell.word), "c"(0), "d"(0)
                               : "memory"); },
            [&]
            {
                // ECX[1] enables the EBX timer. Never issue an unbounded MWAITX.
                asm volatile("mwaitx" ::"a"(0), "c"(2), "b"(1000) : "memory");
            });
        return;
    }
#else
    (void)bell;
    (void)expected;
    (void)kind;
#endif
    WakePause();
}
} // namespace coop::detail
