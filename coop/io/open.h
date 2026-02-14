#pragma once

#include <sys/types.h>

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

bool Open(Handle& handle, const char* path, int flags, mode_t mode = 0);

int Open(const char* path, int flags, mode_t mode = 0);

} // end namespace coop::io
} // end namespace coop
