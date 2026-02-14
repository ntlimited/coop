#pragma once

#include <stddef.h>

namespace coop
{

namespace io
{

struct Descriptor;

// Stream is a transport-agnostic interface for data transfer. It lets application code work with
// both plaintext and TLS connections without committing to either at the call site. Close() is
// intentionally absent — that's a Descriptor concern handled by RAII.
//
struct Stream
{
    virtual ~Stream() = default;
    virtual int Send(const void* buf, size_t size) = 0;
    virtual int SendAll(const void* buf, size_t size) = 0;
    virtual int Recv(void* buf, size_t size) = 0;
};

// PlaintextStream delegates directly to the io:: free functions.
//
struct PlaintextStream : Stream
{
    PlaintextStream(Descriptor& desc) : m_desc(desc) {}

    int Send(const void* buf, size_t size) override;
    int SendAll(const void* buf, size_t size) override;
    int Recv(void* buf, size_t size) override;

    Descriptor& m_desc;
};

} // end namespace coop::io
} // end namespace coop
