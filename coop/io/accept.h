#pragma once

#include "handle.h"

namespace coop
{

namespace io
{

Handle Accept(Context* ctx, Coordinator* coord, Descriptor& desc);

int Accept(Context* ctx, Descriptor& desc);

int Accept(Descriptor& desc);

} // end namespace coop::io
} // end namespace coop
