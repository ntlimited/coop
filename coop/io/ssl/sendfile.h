#pragma once

#include <cerrno>
#include <sys/types.h>

#include "coop/time/interval.h"

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
//   Other:    file read into a bounded buffer, then ssl::Send (fallback)
//
// Returns bytes sent on success, 0 at EOF, -1 on error.
//
int Sendfile(Connection& conn, int in_fd, off_t offset, size_t count);

// Loop Sendfile until all `count` bytes are sent.
//
int SendfileAll(Connection& conn, int in_fd, off_t offset, size_t count);
int SendfileKill(Connection& conn, int in_fd, off_t offset, size_t count);

// Timed cooperative variant: kTLS uses zero-copy splice; other TLS modes stage
// at most 16 KiB. Positive timeout bounds inactivity; the current context's deadline
// supplies an optional overall cap. Returns completed plaintext bytes, zero at EOF,
// or negative errno. A failure is sticky: all later TLS sends return -EPIPE.
//
int SendfileKill(Connection& conn, int in_fd, off_t offset, size_t count,
                 time::Interval timeout);
int SendfileAllKill(Connection& conn, int in_fd, off_t offset, size_t count);

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
