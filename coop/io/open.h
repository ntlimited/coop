#pragma once

#include <sys/types.h>

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Handle;

#define OPEN_ARGS(F) F(const char*, path, ) F(int, flags, ) F(mode_t, mode, = 0)
COOP_IO_URING_DECLARATIONS(Open, OPEN_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef OPEN_ARGS
#endif
