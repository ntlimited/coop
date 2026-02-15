#pragma once

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define SHUTDOWN_ARGS(F) F(int, how, )
COOP_IO_DECLARATIONS(Shutdown, SHUTDOWN_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef SHUTDOWN_ARGS
#endif
