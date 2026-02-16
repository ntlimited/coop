#pragma once

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define POLL_ARGS(F) F(unsigned, mask, )
COOP_IO_DECLARATIONS(Poll, POLL_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef POLL_ARGS
#endif
