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

int Splice(Descriptor& in, Descriptor& out, int pipefd[2], size_t len)
{
    spdlog::trace("splice in={} out={} len={}", in.m_fd, out.m_fd, len);

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
            spdlog::trace("splice in={} EAGAIN", in.m_fd);
            int r = Poll(in, POLLIN);
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
            spdlog::trace("splice out={} EAGAIN", out.m_fd);
            int r = Poll(out, POLLOUT);
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("splice out={} errno={}", out.m_fd, errno);
        return -1;
    }

    spdlog::trace("splice in={} out={} transferred={}", in.m_fd, out.m_fd, n);
    return (int)n;
}

} // end namespace coop::io
} // end namespace coop
