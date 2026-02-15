#include "uring.h"

#include <spdlog/spdlog.h>

#include "handle.h"

#include "coop/context.h"

// Compat defines for kernels/liburing that don't expose these yet. These are stable kernel ABI.
//
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif

namespace coop
{

namespace io
{

void Uring::Init()
{
    unsigned flags = IORING_SETUP_SINGLE_ISSUER;
    if (m_config.sqpoll)
    {
        flags |= IORING_SETUP_SQPOLL;
    }
    if (m_config.iopoll)
    {
        flags |= IORING_SETUP_IOPOLL;
    }
    if (m_config.coopTaskrun)
    {
        flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
    }

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = flags;

    int ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    if (ret == -EINVAL && (flags & IORING_SETUP_SINGLE_ISSUER))
    {
        // SINGLE_ISSUER requires kernel 6.0+. Retry without it but keep user-requested flags.
        //
        flags &= ~IORING_SETUP_SINGLE_ISSUER;
        spdlog::warn("uring init: SINGLE_ISSUER not supported, retrying with flags {:#x}", flags);
        memset(&params, 0, sizeof(params));
        params.flags = flags;
        ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    }
    if (ret == -EINVAL && flags != 0)
    {
        spdlog::warn("uring init with flags {:#x} failed, retrying without", flags);
        memset(&params, 0, sizeof(params));
        ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    }
    assert(ret == 0);
    std::ignore = ret;

    if (!m_registered.empty())
    {
        ret = io_uring_register_files(&m_ring, m_registered.data(), m_registered.size());
        if (ret < 0)
        {
            spdlog::warn("uring register_files failed ret={}", ret);
            m_registered.clear();
        }
    }
}

int Uring::Poll()
{
    int dispatched = 0;
    struct io_uring_cqe* cqe;

    while (io_uring_peek_cqe(&m_ring, &cqe) == 0)
    {
        spdlog::trace("uring cqe result={}", cqe->res);
        Handle::Callback(cqe);
        dispatched++;
    }

    return dispatched;
}

// Work loop for non-native urings that run as a dedicated context
//
void Uring::Run(Context* ctx)
{
    ctx->SetName(m_config.taskName);

    Init();

    while (!ctx->IsKilled())
    {
        ctx->Yield();
        Poll();
    }
}

void Uring::Register(Descriptor* descriptor)
{
    int slots = static_cast<int>(m_registered.size());
    for (int i = 0; i < slots; i++)
    {
        if (m_registered[i] == -1)
        {
            m_registered[i] = descriptor->m_fd;
            int ret = io_uring_register_files_update(&m_ring, i, &descriptor->m_fd, 1);
            if (ret < 0)
            {
                spdlog::warn("uring register_files_update failed fd={} ret={}",
                    descriptor->m_fd, ret);
                m_registered[i] = -1;
                return;
            }
            descriptor->m_registeredIndex = i;
            spdlog::trace("uring register fd={} slot={}", descriptor->m_fd, i);
            return;
        }
    }
    spdlog::debug("uring register fd={} no slot available", descriptor->m_fd);
}

void Uring::Unregister(Descriptor* descriptor)
{
    int idx = descriptor->m_registeredIndex;
    assert(idx >= 0 && idx < static_cast<int>(m_registered.size()));

    int negOne = -1;
    m_registered[idx] = -1;
    int ret = io_uring_register_files_update(&m_ring, idx, &negOne, 1);
    if (ret < 0)
    {
        spdlog::warn("uring unregister_files_update failed fd={} slot={} ret={}",
            descriptor->m_fd, idx, ret);
    }
    descriptor->m_registeredIndex = -1;
    spdlog::trace("uring unregister fd={} slot={}", descriptor->m_fd, idx);
}

} // end namespace io
} // end namespace coop
