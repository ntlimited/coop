#pragma once

#include <sys/types.h>

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Handle;

#define MKDIR_ARGS(F) F(const char*, path, ) F(mode_t, mode, )
COOP_IO_URING_DECLARATIONS(Mkdir, MKDIR_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef MKDIR_ARGS
#endif
