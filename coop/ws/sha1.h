#pragma once

// Self-contained SHA-1 + base64 encode for the WebSocket handshake (RFC 6455).
// No external dependencies. Used only for computing Sec-WebSocket-Accept.
//

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coop
{
namespace ws
{
namespace detail
{

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-4)
// ---------------------------------------------------------------------------

struct SHA1
{
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];

    void Init()
    {
        state[0] = 0x67452301;
        state[1] = 0xEFCDAB89;
        state[2] = 0x98BADCFE;
        state[3] = 0x10325476;
        state[4] = 0xC3D2E1F0;
        count = 0;
        memset(buffer, 0, 64);
    }

    void Update(const void* data, size_t len)
    {
        auto* p = static_cast<const uint8_t*>(data);
        size_t idx = static_cast<size_t>(count & 63);
        count += len;

        while (len > 0)
        {
            size_t fill = 64 - idx;
            size_t n = len < fill ? len : fill;
            memcpy(buffer + idx, p, n);
            idx += n;
            p += n;
            len -= n;
            if (idx == 64)
            {
                Transform(buffer);
                idx = 0;
            }
        }
    }

    void Final(uint8_t digest[20])
    {
        uint64_t bits = count * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        pad = 0;
        while ((count & 63) != 56)
            Update(&pad, 1);

        uint8_t lenBE[8];
        for (int i = 7; i >= 0; i--)
        {
            lenBE[i] = static_cast<uint8_t>(bits);
            bits >>= 8;
        }
        Update(lenBE, 8);

        for (int i = 0; i < 5; i++)
        {
            digest[i * 4 + 0] = static_cast<uint8_t>(state[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
        }
    }

  private:
    static uint32_t RotL(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void Transform(const uint8_t block[64])
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
        {
            w[i] = (uint32_t(block[i * 4]) << 24)
                 | (uint32_t(block[i * 4 + 1]) << 16)
                 | (uint32_t(block[i * 4 + 2]) << 8)
                 | uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++)
            w[i] = RotL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);       k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                 k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;                 k = 0xCA62C1D6; }

            uint32_t t = RotL(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = RotL(b, 30);
            b = a;
            a = t;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }
};

// ---------------------------------------------------------------------------
// Base64 encode (encode direction only)
// ---------------------------------------------------------------------------

inline size_t Base64Encode(const uint8_t* src, size_t srcLen, char* dst)
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t out = 0;
    size_t i = 0;
    while (i + 2 < srcLen)
    {
        uint32_t v = (uint32_t(src[i]) << 16) | (uint32_t(src[i + 1]) << 8) | src[i + 2];
        dst[out++] = kTable[(v >> 18) & 0x3F];
        dst[out++] = kTable[(v >> 12) & 0x3F];
        dst[out++] = kTable[(v >> 6) & 0x3F];
        dst[out++] = kTable[v & 0x3F];
        i += 3;
    }
    if (i < srcLen)
    {
        uint32_t v = uint32_t(src[i]) << 16;
        if (i + 1 < srcLen) v |= uint32_t(src[i + 1]) << 8;
        dst[out++] = kTable[(v >> 18) & 0x3F];
        dst[out++] = kTable[(v >> 12) & 0x3F];
        dst[out++] = (i + 1 < srcLen) ? kTable[(v >> 6) & 0x3F] : '=';
        dst[out++] = '=';
    }
    dst[out] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// Sec-WebSocket-Accept computation (RFC 6455 Section 4.2.2)
// ---------------------------------------------------------------------------
//
// Input:  clientKey — the Sec-WebSocket-Key header value (typically 24 bytes base64)
// Output: out — 28 bytes of base64 (+ null terminator, so >= 29 bytes)
//

inline void ComputeAcceptKey(const char* clientKey, size_t keyLen, char* out)
{
    static constexpr char kGUID[] = "258EAFA5-E914-47DA-95CA-5AB5DC65C37B";

    SHA1 sha;
    sha.Init();
    sha.Update(clientKey, keyLen);
    sha.Update(kGUID, 36);

    uint8_t digest[20];
    sha.Final(digest);
    Base64Encode(digest, 20, out);
}

} // namespace detail
} // namespace coop::ws
} // namespace coop
