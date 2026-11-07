#pragma once

#include <atomic>
#include <cstdint>

namespace coop::detail
{
enum class WakeTier : uint64_t
{
    Hot = 0,
    SemiCold = 1,
    Cold = 2
};
enum class MonitorKind : uint8_t
{
    Pause,
    MonitorX,
    WaitPkg
};

// Generation and state share one modification order; separate atomics would permit a lost wake.
// See docs/tiered_wake_01.md for the cold-transition proof.
struct alignas(64) Doorbell
{
    std::atomic<uint64_t> word{0};

    uint64_t Snapshot() const { return word.load(std::memory_order_seq_cst); }
    static WakeTier Tier(uint64_t value) { return static_cast<WakeTier>(value & 3); }
    WakeTier Notify() { return Tier(word.fetch_add(4, std::memory_order_seq_cst)); }
    bool Enter(uint64_t& expected, WakeTier tier)
    {
        return word.compare_exchange_strong(expected, (expected & ~uint64_t(3)) | uint64_t(tier),
                                            std::memory_order_seq_cst);
    }
    void Hot() { word.fetch_and(~uint64_t(3), std::memory_order_seq_cst); }
};

// Arm address monitoring before checking the generation. A notification in that gap
// must suppress the wait, even on hardware where a missed store would only time out.
template<typename Monitor, typename Wait>
void MonitorThenWait(Doorbell& bell, uint64_t expected, Monitor monitor, Wait wait)
{
    monitor();
    if (bell.Snapshot() != expected)
        return;
    wait();
}

MonitorKind DetectMonitor();
char const* MonitorName(MonitorKind kind);
void MonitorWait(Doorbell& bell, uint64_t expected, MonitorKind kind);
void WakePause();
} // namespace coop::detail
