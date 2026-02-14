#include "send.h"

#include <openssl/ssl.h>

#include "connection.h"

namespace coop
{

namespace io
{

namespace ssl
{

// Send plaintext data over a TLS connection. Mirrors io::Send but operates through OpenSSL's
// encryption layer.
//
// Internally calls SSL_write, which encrypts the plaintext into the write BIO. We then flush the
// resulting ciphertext to the wire. If SSL needs to read (e.g. during renegotiation), we feed
// ciphertext from the wire into the read BIO and retry.
//
// Returns bytes written on success, negative on error, 0 on clean shutdown.
//
int Send(Connection& conn, const void* buf, size_t size)
{
    for (;;)
    {
        int ret = SSL_write(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            // Plaintext was encrypted. Push the ciphertext out.
            //
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_WRITE:
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_WANT_READ:
            // Can happen during TLS renegotiation.
            //
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            if (conn.FeedRead() <= 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_ZERO_RETURN:
            return 0;

        default:
            return -1;
        }
    }
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
