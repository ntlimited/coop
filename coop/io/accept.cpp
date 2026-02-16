#include <sys/socket.h>

#include <spdlog/spdlog.h>

#define COOP_IO_KEEP_ARGS
#include "accept.h"

#include "coop/coordinator.h"
#include "coop/self.h"

#include "descriptor.h"
#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

COOP_IO_IMPLEMENTATIONS(Accept, io_uring_prep_accept, ACCEPT_ARGS)

} // end namespace coop::io
} // end namespace coop
