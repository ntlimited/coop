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

// Note that this takes an IP address, not a DNS name to resolve. Async DNS resolution is
// supremely unfun and someone else can chew that off...
//
int Connect(Descriptor& desc, const char* ipv4, int port);

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef CONNECT_ARGS
#endif
