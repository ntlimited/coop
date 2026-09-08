#pragma once

#include <cstddef>
#include <string_view>

namespace coop
{
namespace http
{

// Bounded element from the request line. String_views point into the recv buffer — valid until
// the next operation that may compact the buffer (advancing phases, reading more data).
//
struct RequestLine
{
    std::string_view method;
    std::string_view version;   // HTTP/1.0 or HTTP/1.1, borrowed like method
    std::string_view path;      // before '?'
    std::string_view query;     // after '?', empty if none
    std::string_view target;    // full request-target (path + '?' + query), verbatim for
                                // forwarding — a proxy sends this as the upstream path
};

// Parsed HTTP response status line. String_view points into the recv buffer.
//
struct ResponseLine
{
    int status;
    std::string_view reason;
};

// Zero-copy chunk from the recv buffer. Returned by read methods for unbounded elements (arg
// values, header values, body). Null pointer return = failure/end.
//
struct Chunk
{
    const void* data;
    size_t size;
    bool complete;              // true if this is the last chunk of the current element
};

// A checked body pull borrows a Chunk from its connection. Truthiness means data;
// Complete() means a successful terminal pull. Error() is a negative errno on failure.
// Like Chunk itself, both the descriptor and its bytes expire on the next parser call.
//
struct BodyResult
{
    Chunk* chunk;
    int error;
    explicit operator bool() const { return chunk != nullptr; }
    Chunk* operator->() const { return chunk; }
    Chunk& operator*() const { return *chunk; }
    bool Complete() const { return !chunk && error == 0; }
    int Error() const { return error; }
};

} // end namespace coop::http
} // end namespace coop
