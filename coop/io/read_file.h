#pragma once

#include <cstddef>

namespace coop
{

namespace io
{

// Read an entire file into a caller-provided buffer. Returns bytes read (>= 0) on success,
// negative errno on error, or -EOVERFLOW if the file exceeds bufSize.
//
int ReadFile(const char* path, void* buf, size_t bufSize);

} // end namespace coop::io
} // end namespace coop
