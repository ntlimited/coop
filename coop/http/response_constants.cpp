#include "response_constants.h"

namespace coop
{
namespace http
{
namespace response
{

#define STATUS_LINE(code, text) \
    static constexpr char SL_##code[] = "HTTP/1.1 " #code " " text "\r\n"

STATUS_LINE(200, "OK");
STATUS_LINE(201, "Created");
STATUS_LINE(204, "No Content");
STATUS_LINE(206, "Partial Content");
STATUS_LINE(301, "Moved Permanently");
STATUS_LINE(302, "Found");
STATUS_LINE(304, "Not Modified");
STATUS_LINE(307, "Temporary Redirect");
STATUS_LINE(308, "Permanent Redirect");
STATUS_LINE(400, "Bad Request");
STATUS_LINE(401, "Unauthorized");
STATUS_LINE(403, "Forbidden");
STATUS_LINE(404, "Not Found");
STATUS_LINE(405, "Method Not Allowed");
STATUS_LINE(408, "Request Timeout");
STATUS_LINE(412, "Precondition Failed");
STATUS_LINE(413, "Payload Too Large");
STATUS_LINE(416, "Range Not Satisfiable");
STATUS_LINE(429, "Too Many Requests");
STATUS_LINE(500, "Internal Server Error");
STATUS_LINE(502, "Bad Gateway");
STATUS_LINE(503, "Service Unavailable");

#undef STATUS_LINE

Fragment StatusLine(int code)
{
    switch (code)
    {
        case 200: return { SL_200, sizeof(SL_200) - 1 };
        case 201: return { SL_201, sizeof(SL_201) - 1 };
        case 204: return { SL_204, sizeof(SL_204) - 1 };
        case 206: return { SL_206, sizeof(SL_206) - 1 };
        case 301: return { SL_301, sizeof(SL_301) - 1 };
        case 302: return { SL_302, sizeof(SL_302) - 1 };
        case 304: return { SL_304, sizeof(SL_304) - 1 };
        case 307: return { SL_307, sizeof(SL_307) - 1 };
        case 308: return { SL_308, sizeof(SL_308) - 1 };
        case 400: return { SL_400, sizeof(SL_400) - 1 };
        case 401: return { SL_401, sizeof(SL_401) - 1 };
        case 403: return { SL_403, sizeof(SL_403) - 1 };
        case 404: return { SL_404, sizeof(SL_404) - 1 };
        case 405: return { SL_405, sizeof(SL_405) - 1 };
        case 408: return { SL_408, sizeof(SL_408) - 1 };
        case 412: return { SL_412, sizeof(SL_412) - 1 };
        case 413: return { SL_413, sizeof(SL_413) - 1 };
        case 416: return { SL_416, sizeof(SL_416) - 1 };
        case 429: return { SL_429, sizeof(SL_429) - 1 };
        case 500: return { SL_500, sizeof(SL_500) - 1 };
        case 502: return { SL_502, sizeof(SL_502) - 1 };
        case 503: return { SL_503, sizeof(SL_503) - 1 };
        default:  return { nullptr, 0 };
    }
}

const char* DefaultReason(int code)
{
    switch (code / 100)
    {
        case 1: return "Informational";
        case 2: return "OK";
        case 3: return "Redirect";
        case 4: return "Client Error";
        case 5: return "Server Error";
        default: return "Status";
    }
}

} // end namespace coop::http::response
} // end namespace coop::http
} // end namespace coop
