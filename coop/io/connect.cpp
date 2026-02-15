#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>

#include <spdlog/spdlog.h>

#define COOP_IO_KEEP_ARGS
#include "connect.h"

#include "coop/coordinator.h"
#include "coop/self.h"

#include "descriptor.h"
#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

COOP_IO_IMPLEMENTATIONS(Connect, io_uring_prep_connect, CONNECT_ARGS)

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
