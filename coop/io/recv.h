#pragma once

#include "handle.h"

namespace coop
{

namespace io
{

Handle Recv(Context*, Coordinator* coord, Descriptor& desc, void* buf, size_t size, int flags = 0);

int Recv(Context* ctx, Descriptor& desc, void* buf, size_t size, int flags = 0);

int Recv(Descriptor& desc, void* buf, size_t size, int flags = 0);

} // end namespace coop::io
} // end namespace coop
