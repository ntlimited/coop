#pragma once

#include <sys/socket.h>

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

bool Accept(Handle& handle, struct sockaddr* addr, socklen_t* addrLen);

int Accept(Descriptor& desc, struct sockaddr* addr = nullptr, socklen_t* addrLen = nullptr);

} // end namespace coop::io
} // end namespace coop
