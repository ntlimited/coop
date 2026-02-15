#pragma once

#include <netinet/in.h>

#include "coop/time/interval.h"

namespace coop
{

namespace io
{

// Resolve a hostname to an IPv4 address. Checks /etc/hosts first, then queries DNS nameservers
// from /etc/resolv.conf over UDP. Returns 0 on success, negative errno on failure.
//
// If hostname is already a dotted-quad numeric address, it is parsed directly without DNS.
//
// Common errors: -ENOENT (not found / NXDOMAIN), -ETIMEDOUT, -EAGAIN (SQE exhaustion)
//
int Resolve4(const char* hostname, struct in_addr* result);
int Resolve4(const char* hostname, struct in_addr* result, time::Interval timeout);

} // end namespace coop::io
} // end namespace coop
