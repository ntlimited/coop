#include "splice.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <spdlog/spdlog.h>

#include "descriptor.h"
#include "poll.h"

namespace coop
{

namespace io
{

static int WaitForSpliceReady(Descriptor& desc, unsigned mask, bool killAware)
{
    if (killAware)
    {
        return PollKill(desc, mask);
    }
    return Poll(desc, mask);
}

static int SpliceImpl(Descriptor& in, Descriptor& out, int pipefd[2], size_t len, bool killAware)
{
    SPDLOG_TRACE("splice in={} out={} len={}", in.m_fd, out.m_fd, len);

    // Phase 1: move data from input socket into pipe
    //
    ssize_t n;
    for (;;)
    {
        n = ::splice(in.m_fd, nullptr, pipefd[1], nullptr, len,
                     SPLICE_F_NONBLOCK | SPLICE_F_MOVE);
        if (n > 0) break;
        if (n == 0) return 0;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            SPDLOG_TRACE("splice in={} EAGAIN", in.m_fd);
            int r = WaitForSpliceReady(in, POLLIN, killAware);
            if (r == -ECANCELED) return -ECANCELED;
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("splice in={} errno={}", in.m_fd, errno);
        return -1;
    }

    // Phase 2: drain pipe into output socket
    //
    size_t remaining = (size_t)n;
    while (remaining > 0)
    {
        ssize_t w = ::splice(pipefd[0], nullptr, out.m_fd, nullptr, remaining,
                             SPLICE_F_NONBLOCK | SPLICE_F_MOVE);
        if (w > 0)
        {
            remaining -= w;
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            SPDLOG_TRACE("splice out={} EAGAIN", out.m_fd);
            int r = WaitForSpliceReady(out, POLLOUT, killAware);
            if (r == -ECANCELED) return -ECANCELED;
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("splice out={} errno={}", out.m_fd, errno);
        return -1;
    }

    SPDLOG_TRACE("splice in={} out={} transferred={}", in.m_fd, out.m_fd, n);
    return (int)n;
}

int Splice(Descriptor& in, Descriptor& out, int pipefd[2], size_t len)
{
    return SpliceImpl(in, out, pipefd, len, false);
}

int SpliceKill(Descriptor& in, Descriptor& out, int pipefd[2], size_t len)
{
    return SpliceImpl(in, out, pipefd, len, true);
}

} // end namespace coop::io
} // end namespace coop
