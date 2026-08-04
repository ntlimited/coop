#define COOP_IO_KEEP_ARGS
#include "read.h"

#include <cerrno>

#include "coop/coordinator.h"
#include "coop/self.h"

#include "descriptor.h"
#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

static inline void PrepRead(struct io_uring_sqe* sqe, int fd, void* buf, size_t size,
                            uint64_t offset, int rwFlags)
{
    io_uring_prep_read(sqe, fd, buf, size, offset);

    // Per-IO rw_flags (RWF_*). RWF_DONTCACHE (6.14) is the target consumer: page-cache
    // riding writes whose pages drop at completion — plumbed now, gated by kernel later.
    //
    sqe->rw_flags = rwFlags;
}

COOP_IO_IMPLEMENTATIONS(Read, PrepRead, READ_ARGS)

static inline void PrepReadFixed(struct io_uring_sqe* sqe, int fd, FixedBuffer buf,
                                 size_t size, uint64_t offset)
{
    io_uring_prep_read_fixed(sqe, fd, buf.data, size, offset, buf.index);
}

COOP_IO_IMPLEMENTATIONS(Read, PrepReadFixed, READ_FIXED_ARGS)

} // end namespace coop::io
} // end namespace coop
