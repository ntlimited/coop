#define COOP_IO_KEEP_ARGS
#include "recv.h"

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

static inline int TryRecv(int fd, void* buf, size_t size, int flags)
{
    return ::recv(fd, buf, size, flags | MSG_DONTWAIT);
}

COOP_IO_IMPLEMENTATIONS_FASTPATH(Recv, io_uring_prep_recv, TryRecv, RECV_ARGS)

} // end namespace coop::io
} // end namespace coop
