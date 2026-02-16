#pragma once

#include <stddef.h>

namespace coop
{

namespace io
{

struct Descriptor;

// Splice up to `len` bytes from `in` to `out` via kernel pipe — zero userspace copies.
// Both descriptors must be non-blocking. The pipe is caller-managed (create with
// pipe2(pipefd, O_NONBLOCK), reuse across calls in a relay loop).
//
// Returns bytes transferred, 0 on EOF (input closed), -1 on error.
//
int Splice(Descriptor& in, Descriptor& out, int pipefd[2], size_t len);

} // end namespace coop::io
} // end namespace coop
