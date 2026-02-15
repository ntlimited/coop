#define COOP_IO_KEEP_ARGS
#include "unlink.h"

#include <cerrno>
#include <fcntl.h>

#include "coop/coordinator.h"
#include "coop/self.h"

#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

COOP_IO_URING_IMPLEMENTATIONS(Unlink, io_uring_prep_unlinkat, UNLINK_ARGS)

} // end namespace coop::io
} // end namespace coop
