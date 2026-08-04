#define COOP_IO_KEEP_ARGS
#include "write.h"

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

static inline void PrepWrite(struct io_uring_sqe* sqe, int fd, const void* buf,
                             size_t size, uint64_t offset, int rwFlags)
{
    io_uring_prep_write(sqe, fd, buf, size, offset);
    sqe->rw_flags = rwFlags;
}

COOP_IO_IMPLEMENTATIONS(Write, PrepWrite, WRITE_ARGS)

static inline void PrepWriteFixed(struct io_uring_sqe* sqe, int fd, FixedBuffer buf,
                                  size_t size, uint64_t offset)
{
    io_uring_prep_write_fixed(sqe, fd, buf.data, size, offset, buf.index);
}

COOP_IO_IMPLEMENTATIONS(Write, PrepWriteFixed, WRITE_FIXED_ARGS)

} // end namespace coop::io
} // end namespace coop
