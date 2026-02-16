#include "sendfile.h"

#include <cerrno>
#include <poll.h>
#include <sys/sendfile.h>
#include <spdlog/spdlog.h>

#include "descriptor.h"
#include "poll.h"

namespace coop
{

namespace io
{

int Sendfile(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    spdlog::trace("sendfile fd={} in_fd={} offset={} count={}", desc.m_fd, in_fd, offset, count);
    for (;;)
    {
        ssize_t ret = ::sendfile(desc.m_fd, in_fd, &offset, count);
        if (ret > 0)
        {
            spdlog::trace("sendfile fd={} sent={}", desc.m_fd, ret);
            return (int)ret;
        }
        if (ret == 0) return 0;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            spdlog::trace("sendfile fd={} EAGAIN", desc.m_fd);
            int r = Poll(desc, POLLOUT);
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("sendfile fd={} errno={}", desc.m_fd, errno);
        return -1;
    }
}

int SendfileAll(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    size_t total = 0;
    while (total < count)
    {
        int sent = Sendfile(desc, in_fd, offset + (off_t)total, count - total);
        if (sent <= 0) return sent;
        total += sent;
    }
    return (int)count;
}

} // end namespace coop::io
} // end namespace coop
