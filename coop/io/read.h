#pragma once

#include <cstdint>

#include "coop/io/detail/op_macros.h"
#include "coop/io/fixed_buffer.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define READ_ARGS(F) F(void*, buf, ) F(size_t, size, ) F(uint64_t, offset, = 0)
COOP_IO_DECLARATIONS(Read, READ_ARGS)

// Fixed-buffer overloads (READ_FIXED): same names, selected by the buffer type.
//
#define READ_FIXED_ARGS(F) F(FixedBuffer, buf, ) F(size_t, size, ) F(uint64_t, offset, = 0)
COOP_IO_DECLARATIONS(Read, READ_FIXED_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef READ_ARGS
#undef READ_FIXED_ARGS
#endif
