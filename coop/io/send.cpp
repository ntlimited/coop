#define COOP_IO_KEEP_ARGS
#include "send.h"

#include <cerrno>
#include <sys/socket.h>

#include "coop/coordinator.h"
#include "coop/self.h"

#include "descriptor.h"
#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

static inline int TrySend(int fd, const void* buf, size_t size, int flags)
{
    return ::send(fd, buf, size, flags | MSG_DONTWAIT);
}

COOP_IO_IMPLEMENTATIONS_FASTPATH(Send, io_uring_prep_send, TrySend, SEND_ARGS)

int SendAll(Descriptor& desc, const void* buf, size_t size, int flags /* = 0 */)
{
    size_t offset = 0;
    while (offset < size)
    {
        int sent = Send(desc, (const char*)buf + offset, size - offset, flags);
        if (sent <= 0)
        {
            return sent;
        }
        offset += sent;
    }
    return (int)size;
}

} // end namespace coop::io
} // end namespace coop
