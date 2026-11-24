#include "sendfile.h"

#include <unistd.h>
#include <spdlog/spdlog.h>

#include "connection.h"
#include "send.h"
#include "coop/io/sendfile.h"
#include "coop/io/read.h"
#include "coop/io/descriptor.h"
#include "coop/io/detail/send_progress.h"
#include "coop/self.h"

namespace coop
{

namespace io
{

namespace ssl
{

static int SendfileImpl(Connection& conn, int in_fd, off_t offset, size_t count, bool killAware)
{
    if (conn.m_writeFailed || conn.m_finalSendAttempted) return -EPIPE;
    // kTLS TX: kernel handles encryption, sendfile() directly — zero copies
    //
    if (conn.m_ktlsTx)
    {
        return killAware
            ? io::SendfileKill(conn.m_desc, in_fd, offset, count)
            : io::Sendfile(conn.m_desc, in_fd, offset, count);
    }

    // Non-kTLS fallback: read from file, encrypt via SSL, send
    //
    char buf[16384];
    size_t toRead = count < sizeof(buf) ? count : sizeof(buf);
    ssize_t n = ::pread(in_fd, buf, toRead, offset);
    if (n < 0)
    {
        spdlog::warn("ssl sendfile pread in_fd={} errno={}", in_fd, errno);
        return -1;
    }
    if (n == 0) return 0;

    return killAware ? ssl::SendKill(conn, buf, n) : ssl::Send(conn, buf, n);
}

int Sendfile(Connection& conn, int in_fd, off_t offset, size_t count)
{
    return SendfileImpl(conn, in_fd, offset, count, false);
}

int SendfileKill(Connection& conn, int in_fd, off_t offset, size_t count)
{
    return SendfileImpl(conn, in_fd, offset, count, true);
}

int SendfileKill(Connection& conn, int in_fd, off_t offset, size_t count,
                 time::Interval timeout)
{
    if (conn.m_writeFailed || conn.m_finalSendAttempted) return -EPIPE;
    if (conn.m_ktlsTx)
    {
        int result = io::SendfileKill(conn.m_desc, in_fd, offset, count, timeout);
        if (result < 0) conn.m_writeFailed = true;
        return result;
    }

    auto fail = [&](int error) { conn.m_writeFailed = true; return error; };
    io::detail::SendProgress progress(Self(), timeout);
    time::Interval remaining;
    if (int error = progress.Prepare(remaining); error < 0) return fail(error);
    char buf[16384];
    size_t size = std::min(count, sizeof(buf));
    io::Descriptor file(io::borrowed, in_fd, conn.m_desc.m_ring);
    int n = remaining.count() > 0
        ? io::ReadKill(file, buf, size, offset, 0, remaining)
        : io::ReadKill(file, buf, size, offset);
    if (n <= 0) return n < 0 ? fail(n) : n;
    if (int error = progress.Prepare(remaining); error < 0) return fail(error);
    return SendImpl(conn, buf, static_cast<size_t>(n), true, timeout, &progress);
}

static int SendfileAllImpl(Connection& conn, int in_fd, off_t offset, size_t count, bool killAware)
{
    if (conn.m_writeFailed || conn.m_finalSendAttempted) return -EPIPE;
    // kTLS TX: kernel handles encryption, sendfile() directly — zero copies
    //
    if (conn.m_ktlsTx)
    {
        return killAware
            ? io::SendfileAllKill(conn.m_desc, in_fd, offset, count)
            : io::SendfileAll(conn.m_desc, in_fd, offset, count);
    }

    // Non-kTLS fallback: read from file in chunks, encrypt and send each
    //
    char buf[16384];
    size_t total = 0;
    while (total < count)
    {
        size_t remaining = count - total;
        size_t toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
        ssize_t n = ::pread(in_fd, buf, toRead, offset + (off_t)total);
        if (n < 0)
        {
            spdlog::warn("ssl sendfile pread in_fd={} errno={}", in_fd, errno);
            return -1;
        }
        if (n == 0) return total > 0 ? (int)total : 0;

        int sent = killAware ? ssl::SendAllKill(conn, buf, n) : ssl::SendAll(conn, buf, n);
        if (sent <= 0) return sent;
        total += n;
    }
    return (int)count;
}

int SendfileAll(Connection& conn, int in_fd, off_t offset, size_t count)
{
    return SendfileAllImpl(conn, in_fd, offset, count, false);
}

int SendfileAllKill(Connection& conn, int in_fd, off_t offset, size_t count)
{
    return SendfileAllImpl(conn, in_fd, offset, count, true);
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
