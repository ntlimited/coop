#pragma once

#include "coop/io/descriptor.h"
#include "coop/io/ssl/connection.h"
#include "coop/io/ssl/recv.h"
#include "coop/io/ssl/send.h"
#include "coop/io/ssl/sendfile.h"
#include "coop/time/interval.h"

namespace coop
{
namespace http
{

// TlsTransport dispatches HTTP I/O through the ssl:: layer. Sendfile dispatches through
// ssl::SendfileAll which uses sendfile() directly for kTLS or falls back to pread+send.
//
struct TlsTransport
{
    TlsTransport(io::ssl::Connection& conn, io::Descriptor& desc)
    : m_conn(conn)
    , m_desc(desc)
    {}

    io::Descriptor& Descriptor() { return m_desc; }

    int Recv(void* buf, size_t size, int /* flags */, time::Interval /* timeout */)
    {
        // TODO: timeout support for TLS recv
        //
        return io::ssl::Recv(m_conn, buf, size);
    }

    int SendAll(const void* buf, size_t size)
    {
        return io::ssl::SendAll(m_conn, buf, size);
    }

    int SendfileAll(int in_fd, off_t offset, size_t count)
    {
        return io::ssl::SendfileAll(m_conn, in_fd, offset, count);
    }

    io::ssl::Connection& m_conn;
    io::Descriptor&      m_desc;
};

} // end namespace coop::http
} // end namespace coop
