#include "stream.h"

#include "recv.h"
#include "send.h"

namespace coop
{

namespace io
{

namespace ssl
{

int SecureStream::Send(const void* buf, size_t size)
{
    return ssl::Send(m_conn, buf, size);
}

int SecureStream::SendAll(const void* buf, size_t size)
{
    return ssl::SendAll(m_conn, buf, size);
}

int SecureStream::Recv(void* buf, size_t size)
{
    return ssl::Recv(m_conn, buf, size);
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
