#define COOP_IO_KEEP_ARGS
#include "mkdir.h"

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

COOP_IO_URING_IMPLEMENTATIONS(Mkdir, io_uring_prep_mkdirat, MKDIR_ARGS)

} // end namespace coop::io
} // end namespace coop
