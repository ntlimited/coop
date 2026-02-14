#pragma once

#include "coop/io/stream.h"

namespace coop
{

namespace io
{

namespace ssl
{

struct Connection;

// SecureStream delegates to the ssl:: free functions, providing transport-agnostic access to
// TLS-encrypted connections through the Stream interface.
//
struct SecureStream : Stream
{
    SecureStream(Connection& conn)
    : m_conn(conn)
    {}

    int Send(const void* buf, size_t size) override;
    int SendAll(const void* buf, size_t size) override;
    int Recv(void* buf, size_t size) override;

    Connection& m_conn;
};

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
