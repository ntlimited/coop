#pragma once

#include <stddef.h>

#include "coop/time/interval.h"

namespace coop
{

namespace io
{

namespace ssl
{

struct Connection;

int Recv(Connection& conn, void* buf, size_t size);
int Recv(Connection& conn, void* buf, size_t size, time::Interval timeout);
int RecvKill(Connection& conn, void* buf, size_t size);
int RecvKill(Connection& conn, void* buf, size_t size, time::Interval timeout);

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
