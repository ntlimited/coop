#pragma once

#include <stddef.h>

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

bool Recv(Handle& handle, void* buf, size_t size, int flags = 0);

int Recv(Descriptor& desc, void* buf, size_t size, int flags = 0);

} // end namespace coop::io
} // end namespace coop
