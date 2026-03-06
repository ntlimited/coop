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
    std::string_view path;      // before '?'
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

} // end namespace coop::http
} // end namespace coop
