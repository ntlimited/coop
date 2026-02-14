#include "stream.h"

#include "recv.h"
#include "send.h"

namespace coop
{

namespace io
{

int PlaintextStream::Send(const void* buf, size_t size)
{
    return io::Send(m_desc, buf, size);
}

int PlaintextStream::SendAll(const void* buf, size_t size)
{
    return io::SendAll(m_desc, buf, size);
}

int PlaintextStream::Recv(void* buf, size_t size)
{
    return io::Recv(m_desc, buf, size);
}

} // end namespace coop::io
} // end namespace coop
