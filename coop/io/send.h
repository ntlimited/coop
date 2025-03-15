#pragma once

#include "handle.h"

namespace coop
{

struct Context;
struct Coordinator;

namespace io
{

struct Descriptor;

Handle Send(Context* ctx, Coordinator* coord, Descriptor& desc, void* buf, size_t size, int flags = 0);

int Send(Context* ctx, Descriptor& desc, void* buf, size_t size, int flags = 0);

int Send(Descriptor& desc, void* buf, size_t size, int flags = 0);

} // end namespace coop::io
} // end namespace coop
