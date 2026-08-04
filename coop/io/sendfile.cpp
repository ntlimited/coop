#include "sendfile.h"

#include <cerrno>
#include <poll.h>
#include <sys/sendfile.h>
#include <spdlog/spdlog.h>

#include "descriptor.h"
#include "poll.h"

#include "coop/context.h"
#include "coop/self.h"

namespace coop
{

namespace io
{

// A peer that never blocks us (fast reader, loopback) keeps sendfile succeeding without
// ever reaching the scheduler through Poll, letting one connection monopolize the
// cooperator. Yield after each budget of unblocked progress — nginx's sendfile_max_chunk
// lesson (its default is likewise 2MB), at a granularity where the scheduler round trip
// is noise.
//
static constexpr size_t kYieldBudgetBytes = 2 * 1024 * 1024;

static int WaitForSendfileWritable(Descriptor& desc, bool killAware)
{
    if (killAware)
    {
        return PollKill(desc, POLLOUT);
    }
    return Poll(desc, POLLOUT);
}

static int SendfileImpl(Descriptor& desc, int in_fd, off_t offset, size_t count, bool killAware)
{
    SPDLOG_TRACE("sendfile fd={} in_fd={} offset={} count={}", desc.m_fd, in_fd, offset, count);
    for (;;)
    {
        ssize_t ret = ::sendfile(desc.m_fd, in_fd, &offset, count);
        if (ret > 0)
        {
            SPDLOG_TRACE("sendfile fd={} sent={}", desc.m_fd, ret);
            return (int)ret;
        }
        if (ret == 0) return 0;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            SPDLOG_TRACE("sendfile fd={} EAGAIN", desc.m_fd);
            int r = WaitForSendfileWritable(desc, killAware);
            if (r == -ECANCELED) return -ECANCELED;
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("sendfile fd={} errno={}", desc.m_fd, errno);
        return -1;
    }
}

int Sendfile(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    return SendfileImpl(desc, in_fd, offset, count, false);
}

int SendfileKill(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    return SendfileImpl(desc, in_fd, offset, count, true);
}

static int SendfileAllImpl(Descriptor& desc, int in_fd, off_t offset, size_t count, bool killAware)
{
    size_t total = 0;
    size_t sinceYield = 0;
    while (total < count)
    {
        int sent = killAware
            ? SendfileKill(desc, in_fd, offset + (off_t)total, count - total)
            : Sendfile(desc, in_fd, offset + (off_t)total, count - total);
        if (sent <= 0) return sent;
        total += sent;

        sinceYield += sent;
        if (sinceYield >= kYieldBudgetBytes && total < count)
        {
            sinceYield = 0;
            Self()->Yield(true);
        }
    }
    return (int)count;
}

int SendfileAll(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    return SendfileAllImpl(desc, in_fd, offset, count, false);
}

int SendfileAllKill(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    return SendfileAllImpl(desc, in_fd, offset, count, true);
}

} // end namespace coop::io
} // end namespace coop
