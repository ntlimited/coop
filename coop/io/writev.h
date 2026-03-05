#pragma once

#include <sys/uio.h>

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

// Scatter-gather write. Submits a single io_uring_prep_writev SQE covering all iovecs.
// For sockets this is equivalent to sendmsg without ancillary data.
//
bool Writev(Handle& handle, const struct iovec* iov, int iovcnt);

int Writev(Descriptor& desc, const struct iovec* iov, int iovcnt);

// Loop Writev until all bytes across all iovecs are sent. Advances through the iovec array
// on partial writes. The iovec array is modified in place (caller must not reuse it).
//
int WritevAll(Descriptor& desc, struct iovec* iov, int iovcnt);

} // end namespace coop::io
} // end namespace coop
