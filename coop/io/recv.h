#pragma once

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define RECV_ARGS(F) F(void*, buf, ) F(size_t, size, ) F(int, flags, = 0)

// Recv submits straight to io_uring. RecvFastpath additionally tries a nonblocking recv() first --
// opt in only when data is usually already buffered at recv time (see recv.cpp for which to use).
//
COOP_IO_DECLARATIONS(Recv, RECV_ARGS)
COOP_IO_DECLARATIONS(RecvFastpath, RECV_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef RECV_ARGS
#endif
