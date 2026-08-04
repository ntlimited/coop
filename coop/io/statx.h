#pragma once

#include <sys/stat.h>

#include "coop/io/detail/op_macros.h"

struct statx;

namespace coop
{

namespace io
{

struct Handle;

// File metadata through the ring (IORING_OP_STATX) — the cooperative replacement for a
// blocking fstat/stat in open->stat->sendfile chains. Metadata is usually dentry-cache hot,
// but a cold inode (network or throttled block storage) stalls a raw syscall for the whole
// cooperator; through the ring it only blocks the calling context.
//
// Path form: Statx("/path", 0, STATX_SIZE, &stx). Result is negative errno on failure.
//
#define STATX_ARGS(F) \
    F(const char*, path, ) F(int, flags, ) F(unsigned, mask, ) F(struct statx*, buf, )
COOP_IO_URING_DECLARATIONS(Statx, STATX_ARGS)

// Fd form (fstat equivalent): StatxFd(fileFd, STATX_SIZE, &stx). Uses AT_EMPTY_PATH under
// the hood. Kill-aware sibling for loops that want prompt cancellation.
//
int StatxFd(int fd, unsigned mask, struct statx* buf);
int StatxFdKill(int fd, unsigned mask, struct statx* buf);

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef STATX_ARGS
#endif
