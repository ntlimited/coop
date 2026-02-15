#pragma once

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define CLOSE_ARGS(F)
COOP_IO_DECLARATIONS(Close, CLOSE_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef CLOSE_ARGS
#endif
