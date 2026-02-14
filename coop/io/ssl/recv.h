#pragma once

#include <stddef.h>

namespace coop
{

namespace io
{

namespace ssl
{

struct Connection;

int Recv(Connection& conn, void* buf, size_t size);

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
