#include "upgrade.h"
#include "sha1.h"

#include "coop/http/connection.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace coop
{
namespace ws
{

namespace
{

bool CaseInsensitiveEq(const char* a, size_t aLen, const char* b, size_t bLen)
{
    if (aLen != bLen) return false;
    for (size_t i = 0; i < aLen; i++)
    {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

// Check if `needle` appears in a comma-separated `haystack` (case-insensitive).
//
bool ContainsToken(const char* haystack, size_t hayLen,
                   const char* needle, size_t needleLen)
{
    size_t pos = 0;
    while (pos < hayLen)
    {
        while (pos < hayLen && (haystack[pos] == ' ' || haystack[pos] == '\t'))
            pos++;

        size_t start = pos;
        while (pos < hayLen && haystack[pos] != ',')
            pos++;

        size_t end = pos;
        while (end > start && (haystack[end - 1] == ' ' || haystack[end - 1] == '\t'))
            end--;

        if (CaseInsensitiveEq(haystack + start, end - start, needle, needleLen))
            return true;

        if (pos < hayLen) pos++;  // skip comma
    }
    return false;
}

} // anonymous namespace

bool Upgrade(http::ConnectionBase& conn)
{
    // Scan HTTP headers for WebSocket upgrade indicators.
    //
    bool hasUpgrade = false;
    bool hasConnection = false;
    bool hasVersion13 = false;
    char wsKey[64] = {};
    size_t wsKeyLen = 0;

    while (const char* name = conn.NextHeaderName())
    {
        auto* val = conn.ReadHeaderValue();
        if (!val) continue;

        size_t nameLen = strlen(name);
        auto* vData = static_cast<const char*>(val->data);
        size_t vLen = val->size;

        if (CaseInsensitiveEq(name, nameLen, "upgrade", 7))
        {
            if (CaseInsensitiveEq(vData, vLen, "websocket", 9))
                hasUpgrade = true;
        }
        else if (CaseInsensitiveEq(name, nameLen, "connection", 10))
        {
            if (ContainsToken(vData, vLen, "upgrade", 7))
                hasConnection = true;
        }
        else if (CaseInsensitiveEq(name, nameLen, "sec-websocket-key", 17))
        {
            wsKeyLen = std::min(vLen, sizeof(wsKey) - 1);
            memcpy(wsKey, vData, wsKeyLen);
        }
        else if (CaseInsensitiveEq(name, nameLen, "sec-websocket-version", 21))
        {
            if (vLen == 2 && vData[0] == '1' && vData[1] == '3')
                hasVersion13 = true;
        }
    }

    if (!hasUpgrade || !hasConnection || wsKeyLen == 0 || !hasVersion13)
    {
        conn.Send(400, "text/plain", "Bad WebSocket handshake\n", 24);
        return false;
    }

    // Compute Sec-WebSocket-Accept.
    //
    char acceptKey[32];
    detail::ComputeAcceptKey(wsKey, wsKeyLen, acceptKey);

    // Send 101 Switching Protocols.
    //
    char response[256];
    int respLen = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", acceptKey);

    return conn.SendRawBytes(response, static_cast<size_t>(respLen));
}

} // namespace coop::ws
} // namespace coop
