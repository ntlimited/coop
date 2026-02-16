#define COOP_IO_KEEP_ARGS
#include "poll.h"

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

COOP_IO_IMPLEMENTATIONS(Poll, io_uring_prep_poll_add, POLL_ARGS)

} // end namespace coop::io
} // end namespace coop
