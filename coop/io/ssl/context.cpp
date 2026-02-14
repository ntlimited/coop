#include "context.h"

#include <cassert>
#include <spdlog/spdlog.h>

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
    spdlog::info("ssl context created mode={}",
        mode == Mode::Server ? "server" : "client");
}

Context::~Context()
{
    SSL_CTX_free(m_ctx);
}

bool Context::LoadCertificate(const char* path)
{
    bool ok = SSL_CTX_use_certificate_chain_file(m_ctx, path) == 1;
    if (ok)
    {
        spdlog::info("ssl loaded certificate path={}", path);
    }
    else
    {
        spdlog::error("ssl failed to load certificate path={}", path);
    }
    return ok;
}

bool Context::LoadPrivateKey(const char* path)
{
    bool ok = SSL_CTX_use_PrivateKey_file(m_ctx, path, SSL_FILETYPE_PEM) == 1;
    if (ok)
    {
        spdlog::info("ssl loaded private key path={}", path);
    }
    else
    {
        spdlog::error("ssl failed to load private key path={}", path);
    }
    return ok;
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
