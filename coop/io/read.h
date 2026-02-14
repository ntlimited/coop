#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

bool Read(Handle& handle, void* buf, size_t size, uint64_t offset = 0);

int Read(Descriptor& desc, void* buf, size_t size, uint64_t offset = 0);

} // end namespace coop::io
} // end namespace coop
