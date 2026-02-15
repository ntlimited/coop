#pragma once

#include <cstdint>

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define WRITE_ARGS(F) F(const void*, buf, ) F(size_t, size, ) F(uint64_t, offset, = 0)
COOP_IO_DECLARATIONS(Write, WRITE_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef WRITE_ARGS
#endif
