#include "context.h"

#include <cassert>

namespace coop
{

namespace io
{

namespace ssl
{

Context::Context(Mode mode)
: m_ctx(SSL_CTX_new(mode == Mode::Server ? TLS_server_method() : TLS_client_method()))
, m_mode(mode)
{
    assert(m_ctx);
}

Context::~Context()
{
    SSL_CTX_free(m_ctx);
}

bool Context::LoadCertificate(const char* path)
{
    return SSL_CTX_use_certificate_chain_file(m_ctx, path) == 1;
}

bool Context::LoadPrivateKey(const char* path)
{
    return SSL_CTX_use_PrivateKey_file(m_ctx, path, SSL_FILETYPE_PEM) == 1;
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
