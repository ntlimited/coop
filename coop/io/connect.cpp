#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <spdlog/spdlog.h>

#include "connect.h"

#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/self.h"

#include "descriptor.h"
#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

bool Connect(Handle& handle, const struct sockaddr* addr, socklen_t addrLen)
{
    auto* sqe = io_uring_get_sqe(&handle.m_descriptor.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    spdlog::trace("connect fd={}", handle.m_descriptor.m_fd);
    io_uring_prep_connect(sqe, handle.m_descriptor.m_fd, addr, addrLen);
    handle.Submit(sqe);
    return true;
}

int Connect(Descriptor& desc, const struct sockaddr* addr, socklen_t addrLen)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Connect(handle, addr, addrLen))
    {
        return -EAGAIN;
    }

    return handle;
}

int Connect(Descriptor& desc, const char* hostname, int port)
{
    spdlog::debug("connect fd={} host={} port={}", desc.m_fd, hostname, port);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    int ret = inet_pton(AF_INET, hostname, &addr.sin_addr);
    if (ret != 1)
    {
        spdlog::warn("connect inet_pton failed host={} ret={}", hostname, ret);
        return ret;
    }

    int result = Connect(desc, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
    spdlog::debug("connect fd={} host={} port={} result={}", desc.m_fd, hostname, port, result);
    return result;
}

} // end namespace coop::io
} // end namespace coop
