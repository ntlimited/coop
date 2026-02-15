#pragma once

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Handle;

#define UNLINK_ARGS(F) F(const char*, path, ) F(int, flags, = 0)
COOP_IO_URING_DECLARATIONS(Unlink, UNLINK_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef UNLINK_ARGS
#endif
