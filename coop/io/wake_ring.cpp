#include "wake_ring.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace coop::io
{
namespace
{
    [[noreturn]] void WakeFailure(char const* operation, int error)
    {
        std::fprintf(stderr, "coop wake ring %s failed: %d\n", operation, error);
        std::abort();
    }
} // namespace

bool WakeRing::Prepare()
{
    std::lock_guard lock(m_lock);
    if (m_initialized)
        return true;
    int ret = io_uring_queue_init(2, &m_sender, 0);
    if (ret < 0)
        return false;
    // Select the COLD transport during construction, before Submit can accept work.
    // Stack-owned probe storage avoids liburing's allocating get_probe convenience API.
    alignas(io_uring_probe) unsigned char
        storage[sizeof(io_uring_probe) + 256 * sizeof(io_uring_probe_op)]{};
    auto* probe = reinterpret_cast<io_uring_probe*>(storage);
    ret = io_uring_register_probe(&m_sender, probe, 256);
    if (ret < 0 || !io_uring_opcode_supported(probe, IORING_OP_MSG_RING))
    {
        io_uring_queue_exit(&m_sender);
        return false;
    }
    m_initialized = true;
    return true;
}

void WakeRing::Init(int target)
{
    std::lock_guard lock(m_lock);
    assert(m_initialized);
    m_target = target;
}

void WakeRing::Close()
{
    std::lock_guard lock(m_lock);
    if (!m_initialized)
        return;
    m_target = -1;
    m_initialized = false;
    io_uring_queue_exit(&m_sender);
}

void WakeRing::Send()
{
    std::lock_guard lock(m_lock);
    if (m_target < 0)
        return; // Before Launch or after its final drain.
    auto* sqe = io_uring_get_sqe(&m_sender);
    if (!sqe)
        WakeFailure("get_sqe", -ENOSPC);
    io_uring_prep_msg_ring(sqe, m_target, 0, 0, 0);
    io_uring_sqe_set_data64(sqe, 0);
    int ret;
    do
    {
        ret = io_uring_submit(&m_sender);
    } while (ret == -EINTR);
    if (ret < 0)
        WakeFailure("submit", ret);
    io_uring_cqe* cqe;
    do
    {
        ret = io_uring_wait_cqe(&m_sender, &cqe);
    } while (ret == -EINTR);
    if (ret < 0)
        WakeFailure("wait", ret);
    int result = cqe->res;
    io_uring_cqe_seen(&m_sender, cqe);
    if (result < 0)
        WakeFailure("delivery", result);
}
} // namespace coop::io
