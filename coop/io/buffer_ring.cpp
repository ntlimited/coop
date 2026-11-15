#include "buffer_ring.h"

#include <cerrno>
#include <cstdlib>
#include <sys/mman.h>

#include <spdlog/spdlog.h>

#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
#include <atomic>
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

        enum class RegistrationMode : uint8_t
        {
            Unknown,
            Standard,
            NonzeroReserved,
        };

        // The accepted reserved-field encoding is a process-wide kernel ABI property. Only a
        // successful registration establishes it; a failure may be specific to one ring or request.
        // Concurrent first registrations may both probe; observations agree on a stable kernel.
        // No ring data is published through this cache. A successful standard retry can update
        // the mode after a live fix restores the standard ABI.
        //
        std::atomic<RegistrationMode> registrationMode{RegistrationMode::Unknown};

        bool IsUbuntu68Family()
        {
            // Bound discovery to the backport family with the inverted reserved-field check.
            // Other kernels may assign a meaning to this reserved field in the future.
            //
            utsname info{};
            return ::uname(&info) == 0 && std::strncmp(info.release, "6.8.0-", 6) == 0 &&
                   std::strstr(info.version, "-Ubuntu") != nullptr;
        }

    } // namespace
#endif

    int BufferRing::Register(Uring& uring)
    {
        int err = 0;
#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
        RegistrationMode const mode = registrationMode.load(std::memory_order_acquire);
        bool const validRequest = m_entries != 0 && m_entries <= 32768 &&
                                  (m_entries & (m_entries - 1)) == 0 && m_bufSize != 0 &&
                                  m_storage.size() == size_t(m_entries) * m_bufSize &&
                                  uring.m_ring.ring_fd >= 0;

        // Keep ordinary liburing setup on healthy kernels. Once the nonzero encoding succeeds,
        // later valid registrations go directly to it, without a doomed registration or uname.
        // Invalid requests still use the ordinary path and cannot establish the alternate mode.
        //
        if (mode != RegistrationMode::NonzeroReserved || !validRequest)
        {
            m_ring = io_uring_setup_buf_ring(&uring.m_ring, m_entries, m_group, 0, &err);
            if (m_ring && mode == RegistrationMode::Unknown)
            {
                registrationMode.store(RegistrationMode::Standard, std::memory_order_release);
            }
        }

        if (!m_ring && validRequest &&
            (mode == RegistrationMode::NonzeroReserved ||
             (mode == RegistrationMode::Unknown && err == -EINVAL && IsUbuntu68Family())))
        {
            // Some Ubuntu backports invert the reserved-field check for both register and
            // unregister (bug 2162843). Within that family, success of this otherwise unchanged
            // request identifies the accepted ABI. All normal argument/group checks still run;
            // healthy kernels reject the nonzero field before it can acquire a buffer group.
            //
            size_t ringSize = size_t(m_entries) * sizeof(io_uring_buf);
            void* mapping =
                mmap(nullptr, ringSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
                    spdlog::critical("buffer-ring failed registration cleanup: errno={}", errno);
                    std::abort();
                }
                // A live fix can restore the standard ABI after discovery. Retry only EINVAL,
                // once; unrelated failures retain their errno and cannot change the cache.
                //
                if (mode != RegistrationMode::NonzeroReserved || ret != -EINVAL)
                {
                    return ret < 0 ? ret : -EIO;
                }
                m_ring = io_uring_setup_buf_ring(&uring.m_ring, m_entries, m_group, 0, &err);
                if (!m_ring)
                {
                    return err;
                }
                registrationMode.store(RegistrationMode::Standard, std::memory_order_release);
            }
            else
            {
                m_ring = static_cast<io_uring_buf_ring*>(mapping);
                io_uring_buf_ring_init(m_ring);
                m_ubuntuWorkaroundRegistered = true;
                if (mode == RegistrationMode::Unknown)
                {
                    registrationMode.store(RegistrationMode::NonzeroReserved,
                                           std::memory_order_release);
                    spdlog::warn("provided-buffer-ring nonzero-reserved ABI detected "
                                 "(Ubuntu bug 2162843 compatibility enabled)");
                }
            }
        }
#else
        m_ring = io_uring_setup_buf_ring(&uring.m_ring, m_entries, m_group, 0, &err);
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
