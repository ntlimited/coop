#pragma once

#include <sys/socket.h>

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define CONNECT_ARGS(F) F(const struct sockaddr*, addr, ) F(socklen_t, addrLen, )
COOP_IO_DECLARATIONS(Connect, CONNECT_ARGS)

// Accepts either a dotted-quad IP address or a hostname. If the string isn't a numeric address,
// DNS resolution is performed cooperatively via Resolve4 (UDP queries through io_uring).
//
int Connect(Descriptor& desc, const char* hostname, int port);

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef CONNECT_ARGS
#endif
