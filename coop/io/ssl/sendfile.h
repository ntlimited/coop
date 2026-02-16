#pragma once

#include <sys/types.h>

namespace coop
{

namespace io
{

namespace ssl
{

struct Connection;

// Send file data over a TLS connection. Dispatches based on connection mode:
//
//   kTLS TX:  sendfile() directly — kernel encrypts, zero userspace copies
//   Other:    pread() into buffer, then ssl::Send (fallback)
//
// Returns bytes sent on success, 0 at EOF, -1 on error.
//
int Sendfile(Connection& conn, int in_fd, off_t offset, size_t count);

// Loop Sendfile until all `count` bytes are sent.
//
int SendfileAll(Connection& conn, int in_fd, off_t offset, size_t count);

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
