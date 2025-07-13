#pragma once

#include <stddef.h>

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

bool Send(Handle& h, void* buf, size_t size, int flags = 0);

int Send(Descriptor& desc, void* buf, size_t size, int flags = 0);

} // end namespace coop::io
} // end namespace coop
