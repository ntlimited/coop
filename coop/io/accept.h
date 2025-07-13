#pragma once

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

bool Accept(Handle& handle, Descriptor& desc);

int Accept(Descriptor& desc);

} // end namespace coop::io
} // end namespace coop
