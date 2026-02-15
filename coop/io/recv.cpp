#define COOP_IO_KEEP_ARGS
#include "recv.h"

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

COOP_IO_IMPLEMENTATIONS(Recv, io_uring_prep_recv, RECV_ARGS)

} // end namespace coop::io
} // end namespace coop
