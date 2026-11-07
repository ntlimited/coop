#pragma once

#include <liburing.h>
#include <mutex>

namespace coop::io
{
// Separate from the target's SINGLE_ISSUER ring. No registered-fd thread affinity,
// and the mutex serializes external senders against both each other and teardown.
class WakeRing
{
public:
    ~WakeRing() { Close(); }
    // Probe once before accepting work; false selects the eventfd transport.
    bool Prepare();
    void Init(int target);
    void Close();
    void Send();

private:
    std::mutex m_lock;
    io_uring m_sender{};
    int m_target{-1};
    bool m_initialized{false};
};
} // namespace coop::io
