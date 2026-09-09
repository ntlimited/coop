#include "upgrade.h"
#include "sha1.h"

#include <cstdio>
#include <cstring>

namespace coop::ws
{
namespace
{
bool CaseInsensitiveEq(const char* a, size_t aLen, const char* b, size_t bLen)
{
    if (aLen != bLen) return false;
    for (size_t i = 0; i < aLen; ++i)
    {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != b[i]) return false;
    }
    return true;
}

// Recognize one token in a streamed comma-separated field. The field may be arbitrarily
// long: only the match position and whitespace phase survive a receive-buffer refill.
struct TokenMatch
{
    const char* token;
    size_t length;
    size_t pos{0};
    bool trailing{false};
    bool mismatch{false};
    bool found{false};

    void Finish()
    {
        found |= !mismatch && pos == length;
        pos = 0;
        trailing = mismatch = false;
    }

    void Feed(const char* data, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
        {
            char c = data[i];
            if (c == ',') { Finish(); continue; }
            if (c == ' ' || c == '\t')
            {
                if (pos) trailing = true;
                continue;
            }
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (trailing || pos >= length || c != token[pos]) mismatch = true;
            if (pos < length) ++pos;
        }
    }
};

// Key/version are protocol-sized single values, with optional surrounding OWS. This
// accumulator rejects overflow instead of truncating it into a seemingly valid value.
struct SmallValue
{
    char* data;
    size_t capacity;
    size_t size{0};
    bool trailing{false};
    bool invalid{false};

    void Feed(const char* input, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            char c = input[i];
            if (c == ' ' || c == '\t')
            {
                if (size) trailing = true;
                continue;
            }
            if (trailing || size == capacity) { invalid = true; continue; }
            data[size++] = c;
        }
    }
};

int Base64Digit(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool ValidKey(const SmallValue& key)
{
    if (key.invalid || key.size != 24 || key.data[22] != '=' || key.data[23] != '=')
        return false;
    for (size_t i = 0; i < 22; ++i)
        if (Base64Digit(key.data[i]) < 0) return false;
    return (Base64Digit(key.data[21]) & 15) == 0;
}
} // namespace

bool Upgrade(http::ConnectionBase& conn, bool respondOnError)
{
    // Make request-line decisions before any header read can invalidate its borrowed views.
    auto* line = conn.GetRequestLine();
    bool valid = line && line->method == "GET" && line->version == "HTTP/1.1";
    bool hasUpgrade = false;
    bool hasConnection = false;
    bool hasHost = false;
    bool sawKey = false;
    bool sawVersion = false;
    char keyData[24];
    SmallValue key{keyData, sizeof(keyData)};
    char versionData[2];
    SmallValue version{versionData, sizeof(versionData)};

    while (const char* name = conn.NextHeaderName())
    {
        // Classify now: the next read may overwrite both name and value storage.
        size_t size = strlen(name);
        enum Field { Other, UpgradeField, ConnectionField, KeyField, VersionField, HostField };
        Field field = Other;
        if (CaseInsensitiveEq(name, size, "upgrade", 7)) field = UpgradeField;
        else if (CaseInsensitiveEq(name, size, "connection", 10)) field = ConnectionField;
        else if (CaseInsensitiveEq(name, size, "sec-websocket-key", 17)) field = KeyField;
        else if (CaseInsensitiveEq(name, size, "sec-websocket-version", 21)) field = VersionField;
        else if (CaseInsensitiveEq(name, size, "host", 4)) field = HostField;

        if (field == Other) { conn.SkipHeaderValue(); continue; }
        if (field == KeyField) { valid &= !sawKey; sawKey = true; }
        if (field == VersionField) { valid &= !sawVersion; sawVersion = true; }
        TokenMatch match{field == UpgradeField ? "websocket" : "upgrade",
                         field == UpgradeField ? size_t(9) : size_t(7)};
        while (auto* chunk = conn.ReadHeaderValue())
        {
            auto* data = static_cast<const char*>(chunk->data);
            if (field == UpgradeField || field == ConnectionField) match.Feed(data, chunk->size);
            else if (field == KeyField) key.Feed(data, chunk->size);
            else if (field == VersionField) version.Feed(data, chunk->size);
            else
                for (size_t i = 0; i < chunk->size; ++i)
                    hasHost |= data[i] != ' ' && data[i] != '\t';
            if (chunk->complete) break;
        }
        match.Finish();
        if (field == UpgradeField) hasUpgrade |= match.found;
        if (field == ConnectionField) hasConnection |= match.found;
    }

    // Never append a 101 (or a second error response) after HTTP framing failed. A body
    // cannot be handed to the frame parser as if it were already WebSocket traffic.
    if (conn.Error() != 0 || conn.ParseError() != 0 || conn.SendError()) return false;
    if (!valid || !hasHost || !hasUpgrade || !hasConnection || !sawKey || !ValidKey(key)
        || !sawVersion || version.invalid || version.size != 2
        || version.data[0] != '1' || version.data[1] != '3' || !conn.Complete())
    {
        conn.ForceClose();
        if (respondOnError)
        {
            constexpr char error[] = "Bad WebSocket handshake\n";
            conn.Send(400, "text/plain", error, sizeof(error) - 1);
        }
        return false;
    }

    char acceptKey[32];
    detail::ComputeAcceptKey(key.data, key.size, acceptKey);
    char response[256];
    int size = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", acceptKey);
    // The HTTP keep-alive loop must never resume parsing after a protocol handoff.
    conn.ForceClose();
    return conn.SendRawBytes(response, static_cast<size_t>(size));
}
} // namespace coop::ws
