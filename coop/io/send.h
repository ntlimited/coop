#pragma once

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define SEND_ARGS(F) F(const void*, buf, ) F(size_t, size, ) F(int, flags, = 0)
COOP_IO_DECLARATIONS(Send, SEND_ARGS)

int SendAll(Descriptor& desc, const void* buf, size_t size, int flags = 0);

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef SEND_ARGS
#endif
