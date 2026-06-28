#define COOP_IO_KEEP_ARGS
#include "send.h"

#include <cerrno>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

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
    // Raw syscall rather than ::send(): glibc's send() is a POSIX cancellation point, so it wraps
    // every call in __pthread_{enable,disable}_asynccancel. coop disables thread cancellation
    // (Launch sets PTHREAD_CANCEL_DISABLE), so that wrapper is pure overhead -- ~14% of a fan-out
    // echo server's CPU in profiling. The raw sendto syscall skips it.
    //
    return (int)syscall(SYS_sendto, fd, buf, size, flags | MSG_DONTWAIT, nullptr, (socklen_t)0);
}

// Send submits straight to io_uring; SendFastpath tries a nonblocking send() first (a win when the
// socket is usually writable, which is the common case for responses). See send.h.
//
COOP_IO_IMPLEMENTATIONS(Send, io_uring_prep_send, SEND_ARGS)
COOP_IO_IMPLEMENTATIONS_FASTPATH(SendFastpath, io_uring_prep_send, TrySend, SEND_ARGS)

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

int SendAllFastpath(Descriptor& desc, const void* buf, size_t size, int flags /* = 0 */)
{
    size_t offset = 0;
    while (offset < size)
    {
        int sent = SendFastpath(desc, (const char*)buf + offset, size - offset, flags);
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
