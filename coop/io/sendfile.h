#pragma once

#include <sys/types.h>

namespace coop
{

namespace io
{

struct Descriptor;

// Send file data directly from a file descriptor to a socket. Uses the sendfile() syscall
// with io::Poll fallback on EAGAIN. The socket must be non-blocking.
//
// Returns bytes sent on success, 0 at EOF, -1 on error.
//
int Sendfile(Descriptor& desc, int in_fd, off_t offset, size_t count);

// Loop Sendfile until all `count` bytes are sent.
//
int SendfileAll(Descriptor& desc, int in_fd, off_t offset, size_t count);

} // end namespace coop::io
} // end namespace coop
