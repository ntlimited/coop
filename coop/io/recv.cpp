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

// Two recv flavors, same io_uring submission underneath. The plain Recv is the straightforward one;
// the fastpath is the special case you opt into deliberately, so a reader of either call site is not
// surprised by a hidden speculative syscall.
//
//   Recv         -- submits straight to io_uring and waits.
//
//   RecvFastpath -- tries a nonblocking recv() first (TryRecv) and only falls to io_uring on EAGAIN.
//                   A win when data is usually already buffered at recv time: keep-alive HTTP with
//                   pipelined requests, pre-staged sends, any read->process->read loop whose
//                   processing gives the peer time to land its next message. A wasted syscall when
//                   data is usually NOT ready -- a strict request/response with negligible turnaround,
//                   where the peer has not sent the next message yet, so the speculative recv() just
//                   EAGAINs every time (a trivial-process ping-pong is exactly this case).
//
COOP_IO_IMPLEMENTATIONS(Recv, io_uring_prep_recv, RECV_ARGS)
COOP_IO_IMPLEMENTATIONS_FASTPATH(RecvFastpath, io_uring_prep_recv, TryRecv, RECV_ARGS)

} // end namespace coop::io
} // end namespace coop
