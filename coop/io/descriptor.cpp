#include "descriptor.h"

#include <spdlog/spdlog.h>

#include "close.h"
#include "uring.h"

#include "coop/context.h"
#include "coop/detail/embedded_list.h"

namespace coop
{

namespace io
{

Descriptor::Descriptor(int fd, Uring* ring /* = GetUring() */)
: m_ring(ring)
, m_fd(fd)
, m_registeredIndex(-1)
{
    assert(m_ring);
    spdlog::debug("descriptor create fd={}", m_fd);
    m_ring->m_descriptors.Push(this);
}

Descriptor::Descriptor(Registered, int fd, Uring* ring /* = GetUring() */)
: m_ring(ring)
, m_fd(fd)
, m_registeredIndex(-1)
{
    assert(m_ring);
    spdlog::debug("descriptor create registered fd={}", m_fd);
    m_ring->m_descriptors.Push(this);
    m_ring->Register(this);
}

Descriptor::~Descriptor()
{
    if (m_registeredIndex >= 0)
    {
        m_ring->Unregister(this);
    }
    if (m_fd >= 0)
    {
        int fd = m_fd;
        int result = Close();
        if (result < 0) [[unlikely]]
        {
            spdlog::warn("descriptor close in destructor failed fd={} result={}", fd, result);
        }
    }
    m_ring->m_descriptors.Remove(this);
}

int Descriptor::Close()
{
    if (m_fd < 0)
    {
        return 0;
    }
    spdlog::debug("descriptor close fd={}", m_fd);
    int result = io::Close(*this);
    m_fd = -1;
    return result;
}

} // end namespace io
} // end namespace coop
