#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

namespace coop
{
struct Context;

// One waiter, N arrivals, one completion. Arrive may run on any thread; Wait runs
// on a cooperator. Storage must outlive every arrival and Wait. Reset requires
// all prior users joined and must precede publication of a fresh round.
// No allocation or per-latch kernel object is needed. Arrival publishes preceding
// writes to the waiter, including when the last arrival precedes Wait.
class CountdownLatch
{
public:
    CountdownLatch() = default;
    explicit CountdownLatch(uint32_t count) : m_remaining(count) { assert(count < kWaiter); }
    CountdownLatch(CountdownLatch const&) = delete;
    CountdownLatch& operator=(CountdownLatch const&) = delete;
    ~CountdownLatch() { assert(!m_registration); }

    void Reset(uint32_t count);
    void Arrive();
    bool IsReady() const;
    void Wait(Context* ctx);

    enum class WaitStatus
    {
        Completed,
        Killed,
        Error
    };

    // On cancellation or a coordination error, call onCancel once on the waiter,
    // then drain all arrivals before returning that status. Notification storage
    // and accepted work cannot be abandoned. onCancel must not throw.
    // afterRegistration observes a published pending wait before suspension,
    // for diagnostics and deterministic scheduling tests; it may yield but not throw.
    WaitStatus WaitKill(Context* ctx, void (*onCancel)(void*, WaitStatus) = nullptr,
                        void* state = nullptr, void (*afterRegistration)(void*) = nullptr);

private:
    struct Notification;
    WaitStatus WaitImpl(Context*, bool killAware, void (*onCancel)(void*, WaitStatus), void*,
                        void (*afterRegistration)(void*));
    static constexpr uint32_t kWaiter = uint32_t{1} << 31;
    std::atomic<uint32_t> m_remaining{0};
    Notification* m_registration{nullptr};
};
} // namespace coop
