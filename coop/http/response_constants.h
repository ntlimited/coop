#pragma once

#include <cstddef>

namespace coop
{
namespace http
{
namespace response
{

struct Fragment
{
    const char* data;
    size_t size;
};

// Pre-compiled HTTP/1.1 status lines including trailing \r\n.
//
Fragment StatusLine(int code);

// Header name prefixes — used with AppendLiteral for zero-overhead memcpy.
//
inline constexpr char CONTENT_TYPE[]     = "Content-Type: ";
inline constexpr char CONTENT_LENGTH[]   = "\r\nContent-Length: ";
inline constexpr char TRANSFER_ENCODING_CHUNKED[] =
    "Transfer-Encoding: chunked\r\n";

// Connection header + final blank line. Terminates the header block.
//
inline constexpr char CONN_KEEP_ALIVE[]  = "Connection: keep-alive\r\n\r\n";
inline constexpr char CONN_CLOSE[]       = "Connection: close\r\n\r\n";

inline constexpr char CRLF[] = "\r\n";
inline constexpr char CHUNKED_TERMINATOR[] = "0\r\n\r\n";

} // end namespace coop::http::response
} // end namespace coop::http
} // end namespace coop
