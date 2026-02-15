#pragma once

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define FSYNC_ARGS(F) F(unsigned, flags, = 0)
COOP_IO_DECLARATIONS(Fsync, FSYNC_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef FSYNC_ARGS
#endif
