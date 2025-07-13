#pragma once

namespace coop
{

struct Context;
struct Coordinator;

namespace io
{

struct Descriptor;
struct Handle;

bool Connect(Handle& handle, const struct sockaddr*, socklen_t addrLen);

int Connect(Descriptor& desc, const struct sockaddr* addr, socklen_t addrLen);

// Note that this takes an IP address, not a DNS name to resolve. Async DNS resolution is
// supremely unfun and someone else can chew that off...
//
int Connect(Descriptor& desc, const char* ipv4, int port);

} // end namespace coop::io
} // end namespace coop
