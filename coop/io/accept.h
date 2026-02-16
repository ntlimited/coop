#pragma once

#include <sys/socket.h>

#include "coop/io/detail/op_macros.h"

namespace coop
{

namespace io
{

struct Descriptor;
struct Handle;

#define ACCEPT_ARGS(F) \
    F(struct sockaddr*, addr, = nullptr) F(socklen_t*, addrLen, = nullptr) F(int, flags, = 0)
COOP_IO_DECLARATIONS(Accept, ACCEPT_ARGS)

} // end namespace coop::io
} // end namespace coop

#ifndef COOP_IO_KEEP_ARGS
#undef ACCEPT_ARGS
#endif
