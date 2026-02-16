#include "sendfile.h"

#include <unistd.h>
#include <spdlog/spdlog.h>

#include "connection.h"
#include "send.h"
#include "coop/io/sendfile.h"

namespace coop
{

namespace io
{

namespace ssl
{

int Sendfile(Connection& conn, int in_fd, off_t offset, size_t count)
{
    // kTLS TX: kernel handles encryption, sendfile() directly — zero copies
    //
    if (conn.m_ktlsTx)
    {
        return io::Sendfile(conn.m_desc, in_fd, offset, count);
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

    return ssl::Send(conn, buf, n);
}

int SendfileAll(Connection& conn, int in_fd, off_t offset, size_t count)
{
    // kTLS TX: kernel handles encryption, sendfile() directly — zero copies
    //
    if (conn.m_ktlsTx)
    {
        return io::SendfileAll(conn.m_desc, in_fd, offset, count);
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

        int sent = ssl::SendAll(conn, buf, n);
        if (sent <= 0) return sent;
        total += n;
    }
    return (int)count;
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
