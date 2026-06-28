#pragma once

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define SEND_ARGS(F) F(const void*, buf, ) F(size_t, size, ) F(int, flags, = 0)

// Send submits straight to io_uring. SendFastpath additionally tries a nonblocking send() first --
// opt in when the socket is usually writable at send time (the common case for responses), so it
// completes without an io_uring round trip. Plain Send is the unsurprising default.
//
COOP_IO_DECLARATIONS(Send, SEND_ARGS)
COOP_IO_DECLARATIONS(SendFastpath, SEND_ARGS)

// SendAll loops until the whole buffer is written. SendAllFastpath does the same over SendFastpath.
//
int SendAll(Descriptor& desc, const void* buf, size_t size, int flags = 0);
int SendAllFastpath(Descriptor& desc, const void* buf, size_t size, int flags = 0);

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef SEND_ARGS
#endif
