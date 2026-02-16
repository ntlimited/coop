#include "context.h"

#include <cassert>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>

#include <spdlog/spdlog.h>

namespace coop
{

namespace io
{

namespace ssl
{

// Suppress OpenSSL's atexit handler. SSL objects are created on the cooperator thread, which joins
// before process exit. OPENSSL_cleanup's atexit handler then accesses freed per-thread state and
// crashes. Must be called before the first SSL_CTX_new (which internally calls OPENSSL_init_ssl
// and would register the atexit handler). Idempotent.
//
static SSL_CTX* CreateCtx(Mode mode)
{
    static bool initialized = [] {
        OPENSSL_init_ssl(OPENSSL_INIT_NO_ATEXIT, nullptr);
        return true;
    }();
    (void)initialized;

    return SSL_CTX_new(mode == Mode::Server ? TLS_server_method() : TLS_client_method());
}

Context::Context(Mode mode)
: m_ctx(CreateCtx(mode))
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

bool Context::LoadCertificate(const char* pem, size_t len)
{
    BIO* bio = BIO_new_mem_buf(pem, static_cast<int>(len));
    if (!bio)
    {
        spdlog::error("ssl failed to create bio for certificate");
        return false;
    }

    // First certificate in the chain is the leaf
    //
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    if (!cert)
    {
        spdlog::error("ssl failed to parse leaf certificate");
        BIO_free(bio);
        return false;
    }

    if (SSL_CTX_use_certificate(m_ctx, cert) != 1)
    {
        spdlog::error("ssl failed to set leaf certificate");
        X509_free(cert);
        BIO_free(bio);
        return false;
    }
    X509_free(cert);

    // Remaining certificates are intermediates in the chain
    //
    ERR_clear_error();
    X509* chain;
    while ((chain = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) != nullptr)
    {
        if (SSL_CTX_add_extra_chain_cert(m_ctx, chain) != 1)
        {
            spdlog::error("ssl failed to add chain certificate");
            X509_free(chain);
            BIO_free(bio);
            return false;
        }
        // SSL_CTX_add_extra_chain_cert takes ownership on success, do not free
        //
    }

    BIO_free(bio);
    spdlog::info("ssl loaded certificate chain len={}", len);
    return true;
}

bool Context::LoadPrivateKey(const char* pem, size_t len)
{
    BIO* bio = BIO_new_mem_buf(pem, static_cast<int>(len));
    if (!bio)
    {
        spdlog::error("ssl failed to create bio for private key");
        return false;
    }

    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!key)
    {
        spdlog::error("ssl failed to parse private key");
        return false;
    }

    int ret = SSL_CTX_use_PrivateKey(m_ctx, key);
    EVP_PKEY_free(key);

    if (ret != 1)
    {
        spdlog::error("ssl failed to set private key");
        return false;
    }

    spdlog::info("ssl loaded private key len={}", len);
    return true;
}

void Context::EnableKTLS()
{
    SSL_CTX_set_options(m_ctx, SSL_OP_ENABLE_KTLS);
    spdlog::info("ssl ktls enabled");
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
