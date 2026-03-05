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
STATUS_LINE(301, "Moved Permanently");
STATUS_LINE(302, "Found");
STATUS_LINE(304, "Not Modified");
STATUS_LINE(400, "Bad Request");
STATUS_LINE(401, "Unauthorized");
STATUS_LINE(403, "Forbidden");
STATUS_LINE(404, "Not Found");
STATUS_LINE(405, "Method Not Allowed");
STATUS_LINE(408, "Request Timeout");
STATUS_LINE(413, "Payload Too Large");
STATUS_LINE(500, "Internal Server Error");
STATUS_LINE(502, "Bad Gateway");
STATUS_LINE(503, "Service Unavailable");

#undef STATUS_LINE

static constexpr char SL_UNKNOWN[] = "HTTP/1.1 0 Unknown\r\n";

Fragment StatusLine(int code)
{
    switch (code)
    {
        case 200: return { SL_200, sizeof(SL_200) - 1 };
        case 201: return { SL_201, sizeof(SL_201) - 1 };
        case 204: return { SL_204, sizeof(SL_204) - 1 };
        case 301: return { SL_301, sizeof(SL_301) - 1 };
        case 302: return { SL_302, sizeof(SL_302) - 1 };
        case 304: return { SL_304, sizeof(SL_304) - 1 };
        case 400: return { SL_400, sizeof(SL_400) - 1 };
        case 401: return { SL_401, sizeof(SL_401) - 1 };
        case 403: return { SL_403, sizeof(SL_403) - 1 };
        case 404: return { SL_404, sizeof(SL_404) - 1 };
        case 405: return { SL_405, sizeof(SL_405) - 1 };
        case 408: return { SL_408, sizeof(SL_408) - 1 };
        case 413: return { SL_413, sizeof(SL_413) - 1 };
        case 500: return { SL_500, sizeof(SL_500) - 1 };
        case 502: return { SL_502, sizeof(SL_502) - 1 };
        case 503: return { SL_503, sizeof(SL_503) - 1 };
        default:  return { SL_UNKNOWN, sizeof(SL_UNKNOWN) - 1 };
    }
}

} // end namespace coop::http::response
} // end namespace coop::http
} // end namespace coop
