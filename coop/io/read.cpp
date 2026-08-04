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

COOP_IO_IMPLEMENTATIONS(Read, io_uring_prep_read, READ_ARGS)

static inline void PrepReadFixed(struct io_uring_sqe* sqe, int fd, FixedBuffer buf,
                                 size_t size, uint64_t offset)
{
    io_uring_prep_read_fixed(sqe, fd, buf.data, size, offset, buf.index);
}

COOP_IO_IMPLEMENTATIONS(Read, PrepReadFixed, READ_FIXED_ARGS)

} // end namespace coop::io
} // end namespace coop
