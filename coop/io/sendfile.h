#pragma once

#include <sys/types.h>

#include "coop/time/interval.h"

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

// Kill-aware variants. Return -ECANCELED when kill wins while waiting for socket writability.
//
int SendfileKill(Descriptor& desc, int in_fd, off_t offset, size_t count);

// Timed partial progress: at most 16 KiB through a pooled zero-copy splice path.
// Source page acquisition runs asynchronously. kTLS stages at most 16383 bytes and
// flushes records with MSG_NOSIGNAL. Positive timeout bounds inactivity;
// zero adds no inactivity limit. Every wait observes kill and the current context's
// deadline. Returns completed socket bytes, zero at EOF, or negative errno.
// Closed output returns -EPIPE without raising SIGPIPE. A negative result may follow
// a partial write; callers must terminate the stream.
//
int SendfileKill(Descriptor& desc, int in_fd, off_t offset, size_t count,
                 time::Interval timeout);
int SendfileAllKill(Descriptor& desc, int in_fd, off_t offset, size_t count);

} // end namespace coop::io
} // end namespace coop
