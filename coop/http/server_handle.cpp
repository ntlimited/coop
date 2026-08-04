#include "server_handle.h"

#include <sys/socket.h>

#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/io/descriptor.h"

namespace coop
{

namespace http
{

void ServerHandle::ShutdownAllConns(int how)
{
    // Raw shutdown(2): a synchronous, non-blocking control-plane syscall (it never
    // waits), so the fan-out over every live connection stays cheap and needs no ring
    // op. Walk without mutating — connections deregister themselves as they exit.
    //
    for (ConnNode* node = m_conns.IsEmpty() ? nullptr : m_conns.Peek();
         node; node = m_conns.Next(node))
    {
        if (node->desc && node->desc->m_fd >= 0)
        {
            ::shutdown(node->desc->m_fd, how);
        }
    }
}

bool ServerHandle::Drain(Context* ctx, time::Interval timeout)
{
    m_draining = true;

    // Stop accepting: shut the listen socket so the accept loop's blocking accept
    // returns an error and the acceptor unwinds. New connections are refused by the
    // kernel from here on.
    //
    if (m_listener && m_listener->m_fd >= 0)
    {
        ::shutdown(m_listener->m_fd, SHUT_RDWR);
    }

    if (m_liveCount == 0)
    {
        return true;
    }

    // Soft phase: wake idle keep-alive connections (blocked reading their next request)
    // by shutting the read side — they see EOF and exit; connections mid-response finish
    // with Connection: close (the drain flag forces it). Then wait for the count to
    // reach zero, bounded by the deadline.
    //
    ShutdownAllConns(SHUT_RD);

    m_drainWait.TryAcquire(ctx);
    auto result = CoordinateWith(ctx, &m_drainWait, timeout);

    if (result.TimedOut())
    {
        // Hard phase: fully shut every remaining connection, waking any blocked IO with
        // an error, then wait for them to unwind. The coordinator is still held from the
        // soft-phase TryAcquire; the last deregister releases it.
        //
        ShutdownAllConns(SHUT_RDWR);
        if (m_liveCount > 0)
        {
            CoordinateWith(ctx, &m_drainWait);
        }
        else if (m_drainWait.IsHeld())
        {
            m_drainWait.Release(ctx, false);
        }
        return false;
    }

    return true;
}

} // end namespace coop::http
} // end namespace coop
