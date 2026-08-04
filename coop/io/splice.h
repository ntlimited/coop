#pragma once

#include <stddef.h>
#include <sys/types.h>

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

// Kill-aware variant of Splice. Returns -ECANCELED when kill wins while waiting for readiness.
//
int SpliceKill(Descriptor& in, Descriptor& out, int pipefd[2], size_t len);

// Splice up to `len` bytes from a socket into a regular file at `offset` — zero userspace
// copies (socket -> kernel pipe -> page cache). The file leg never sleeps cooperatively:
// regular-file splice writes complete synchronously against the page cache (dirty-throttling
// stalls are possible under extreme writeback pressure, the same trade a buffered write
// makes). A partially drained pipe on failure is the caller's to discard — with a PipeLease,
// MarkDirty() on any negative return.
//
// Returns bytes transferred, 0 on EOF (socket closed), negative errno on error.
//
int SpliceToFile(Descriptor& in, int fileFd, off_t offset, int pipefd[2], size_t len);

// Kill-aware variant. Returns -ECANCELED when kill wins while waiting for socket readiness.
//
int SpliceToFileKill(Descriptor& in, int fileFd, off_t offset, int pipefd[2], size_t len);

} // end namespace coop::io
} // end namespace coop
