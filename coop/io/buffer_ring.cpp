#include "buffer_ring.h"

#include <cerrno>
#include <cstdlib>
#include <sys/mman.h>

#include <spdlog/spdlog.h>

#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
#include <cstring>
#include <sys/utsname.h>
#endif

namespace coop
{

namespace io
{

#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
    namespace
    {

        bool AffectedUbuntuKernel(utsname* kernel)
        {
            if (uname(kernel) != 0)
            {
                return false;
            }
            return (std::strcmp(kernel->release, "6.8.0-139-generic") == 0 ||
                    std::strcmp(kernel->release, "6.8.0-142-generic") == 0) &&
                   std::strstr(kernel->version, "-Ubuntu") != nullptr;
        }

    } // namespace
#endif

    int BufferRing::Register(Uring& uring)
    {
        int err = 0;
        m_ring = io_uring_setup_buf_ring(&uring.m_ring, m_entries, m_group, 0, &err);

#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
        if (!m_ring && err == -EINVAL)
        {
            utsname kernel{};
            // The ordinary helper has already rejected this request. Retry only a valid userspace
            // ring on the two Ubuntu builds known to invert the reserved-field check.
            //
            if (m_entries != 0 && m_entries <= 32768 && (m_entries & (m_entries - 1)) == 0 &&
                m_bufSize != 0 && m_storage.size() == size_t(m_entries) * m_bufSize &&
                uring.m_ring.ring_fd >= 0 && AffectedUbuntuKernel(&kernel))
            {
                size_t ringSize = size_t(m_entries) * sizeof(io_uring_buf);
                void* mapping = mmap(nullptr, ringSize, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (mapping == MAP_FAILED)
                {
                    return -errno;
                }

                io_uring_buf_reg reg{};
                reg.ring_addr = reinterpret_cast<uintptr_t>(mapping);
                reg.ring_entries = m_entries;
                reg.bgid = m_group;
                reg.resv[0] = 1;
                int ret = io_uring_register_buf_ring(&uring.m_ring, &reg, 0);
                if (ret != 0)
                {
                    if (munmap(mapping, ringSize) != 0)
                    {
                        spdlog::critical("buffer-ring failed registration cleanup: errno={}",
                                         errno);
                        std::abort();
                    }
                    return ret < 0 ? ret : -EIO;
                }

                m_ring = static_cast<io_uring_buf_ring*>(mapping);
                io_uring_buf_ring_init(m_ring);
                m_ubuntuWorkaroundRegistered = true;
                spdlog::warn("Ubuntu provided-buffer-ring workaround active on kernel {} "
                             "(Ubuntu bug 2162843)",
                             kernel.release);
            }
        }
#endif

        if (!m_ring)
        {
            return err;
        }
        m_uring = &uring;
        for (uint32_t b = 0; b < m_entries; b++)
        {
            io_uring_buf_ring_add(m_ring, Slot(b), m_bufSize, b, m_mask, b);
        }
        io_uring_buf_ring_advance(m_ring, m_entries);
        return 0;
    }

    BufferRing::~BufferRing()
    {
        if (!m_ring || !m_uring)
        {
            return;
        }

        int ret = 0;
#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
        if (m_ubuntuWorkaroundRegistered)
        {
            io_uring_buf_reg reg{};
            reg.bgid = m_group;
            reg.resv[0] = 1;
            // Uring retains the original fd until queue_exit, even when enter and
            // registration use liburing's task-local registered index. The public
            // low-level wrapper lets this request carry nonzero reserved fields.
            //
            ret = io_uring_register(m_uring->m_ring.ring_fd, IORING_UNREGISTER_PBUF_RING, &reg, 1);
            if (ret == 0 && munmap(m_ring, size_t(m_entries) * sizeof(io_uring_buf)) != 0)
            {
                ret = -errno;
            }
        }
        else
#endif
        {
            ret = io_uring_free_buf_ring(&m_uring->m_ring, m_ring, m_entries, m_group);
        }
        if (ret != 0)
        {
            // liburing leaves the mapping intact when unregistration fails. Stop before the
            // backing storage is destroyed while the kernel may still hold its addresses.
            //
            spdlog::critical("buffer-ring unregister/cleanup failed group={} ret={}", m_group, ret);
            std::abort();
        }
    }

} // namespace io
} // namespace coop
