#include "recv.h"

#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include "connection.h"
#include "coop/io/descriptor.h"

namespace coop
{

namespace io
{

namespace ssl
{

// Receive plaintext data from a TLS connection. Mirrors io::Recv but operates through OpenSSL's
// decryption layer.
//
// Internally calls SSL_read, which decrypts ciphertext from the read BIO into plaintext. If SSL
// needs more ciphertext, we pull it from the wire via FeedRead and retry. If SSL needs to write
// (e.g. during renegotiation), we flush and retry.
//
// Returns bytes read on success, negative on error, 0 on clean shutdown.
//
int Recv(Connection& conn, void* buf, size_t size)
{
    spdlog::trace("ssl recv fd={} maxsize={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_read(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            spdlog::trace("ssl recv fd={} read={}", conn.m_desc.m_fd, ret);
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_READ:
            spdlog::trace("ssl recv fd={} WANT_READ", conn.m_desc.m_fd);
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            if (conn.FeedRead() <= 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_WANT_WRITE:
            // Can happen during TLS renegotiation.
            //
            spdlog::trace("ssl recv fd={} WANT_WRITE", conn.m_desc.m_fd);
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_ZERO_RETURN:
            return 0;

        default:
            spdlog::warn("ssl recv fd={} error={}", conn.m_desc.m_fd, err);
            return -1;
        }
    }
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
