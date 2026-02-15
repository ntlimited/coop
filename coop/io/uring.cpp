#include "uring.h"

#include <spdlog/spdlog.h>

#include "handle.h"

#include "coop/context.h"

namespace coop
{

namespace io
{

void Uring::Init()
{
    int ret = io_uring_queue_init(m_entries, &m_ring, 0);
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

// Work loop
//
void Uring::Run(Context* ctx)
{
    Init();
    while (!ctx->IsKilled())
    {
        ctx->Yield();

        struct io_uring_cqe* cqe;
        int ret = io_uring_peek_cqe(&m_ring, &cqe);
        if (ret == -EAGAIN || ret == -EINTR)
        {
            continue;
        }
        if (ret < 0)
        {
            assert(false);
        }
        assert(ret == 0);
        spdlog::trace("uring cqe result={}", cqe->res);
        Handle::Callback(cqe);
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
