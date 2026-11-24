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

// One immediate write attempt; never waits and terminates further TLS writes.
int TrySend(Connection& conn, const void* buf, size_t size);

int Send(Connection& conn, const void* buf, size_t size);
int SendKill(Connection& conn, const void* buf, size_t size);

// Positive timeout bounds socket-write inactivity, including encrypted-output flushes.
// Each call returns at most 16 KiB of completed plaintext progress. A send failure
// terminates the TLS write stream; encrypted staging bytes must not be retried.
//
int SendKill(Connection& conn, const void* buf, size_t size, time::Interval timeout);

int SendAll(Connection& conn, const void* buf, size_t size);
int SendAllKill(Connection& conn, const void* buf, size_t size);

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
