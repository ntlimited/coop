#pragma once

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

bool Close(Handle& handle);

int Close(Descriptor& desc);

} // end namespace coop::io
} // end namespace coop
