#pragma once

#include <stddef.h>

#include "coop/time/interval.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

bool Recv(Handle& handle, void* buf, size_t size, int flags = 0);

int Recv(Descriptor& desc, void* buf, size_t size, int flags = 0);

// Recv with a linked timeout. Returns -ETIMEDOUT if the timeout fires before data arrives.
//
int Recv(Descriptor& desc, void* buf, size_t size, time::Interval timeout, int flags = 0);

} // end namespace coop::io
} // end namespace coop
