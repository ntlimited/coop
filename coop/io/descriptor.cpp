#include "descriptor.h"

#include <spdlog/spdlog.h>

#include "close.h"
#include "uring.h"

#include "coop/context.h"
#include "coop/detail/embedded_list.h"
#include "coop/self.h"

namespace coop
{

namespace io
{

Descriptor::Descriptor(int fd, Uring* ring /* = nullptr */)
: m_ring(ring ? ring : GetUring())
, m_fd(fd)
, m_registered(nullptr)
, m_generation(0)
{
    assert(m_ring);
    spdlog::debug("descriptor create fd={}", m_fd);
    m_ring->Register(this);
}

Descriptor::~Descriptor()
{
    if (m_fd >= 0)
    {
        int fd = m_fd;
        int result = Close();
        if (result < 0) [[unlikely]]
        {
            spdlog::warn("descriptor close in destructor failed fd={} result={}", fd, result);
        }
    }
    m_ring->Unregister(this);
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
