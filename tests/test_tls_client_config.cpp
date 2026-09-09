#include <gtest/gtest.h>

#include <climits>
#include <memory>
#include <string>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include "coop/io/descriptor.h"
#include "coop/io/ssl/connection.h"
#include "coop/io/ssl/context.h"
#include "coop/io/uring.h"

namespace
{

namespace ssl = coop::io::ssl;

class TlsClientConfigTest : public testing::Test
{
  protected:
    // The ring is deliberately never initialized. Borrowed descriptors and memory BIO TLS
    // objects need no kernel operations; the test drives encrypted records directly between
    // OpenSSL BIOs to exercise authentication independently of scheduler/network availability.
    //
    coop::io::Uring m_ring;
    coop::io::Descriptor m_descriptor{coop::io::borrowed, -1, &m_ring};
    ssl::Context m_server{ssl::Mode::Server};
    ssl::Context m_client{ssl::Mode::Client};
    std::string m_certificate;
    char m_serverBuffer[ssl::Connection::BUFFER_SIZE];
    char m_clientBuffer[ssl::Connection::BUFFER_SIZE];
    std::unique_ptr<ssl::Connection> m_serverConnection;
    std::unique_ptr<ssl::Connection> m_clientConnection;

    void SetUp() override
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        ASSERT_NE(generator, nullptr);
        ASSERT_EQ(EVP_PKEY_keygen_init(generator.get()), 1);
        ASSERT_EQ(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(),
            NID_X9_62_prime256v1), 1);
        EVP_PKEY* rawKey = nullptr;
        ASSERT_EQ(EVP_PKEY_keygen(generator.get(), &rawKey), 1);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        ASSERT_NE(cert, nullptr);
        ASSERT_EQ(X509_set_version(cert.get(), 2), 1);
        ASSERT_EQ(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1), 1);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600), nullptr);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400), nullptr);
        ASSERT_EQ(X509_set_pubkey(cert.get(), key.get()), 1);
        X509_NAME* subject = X509_get_subject_name(cert.get());
        ASSERT_EQ(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("server.test"), -1, -1, 0), 1);
        ASSERT_EQ(X509_set_issuer_name(cert.get(), subject), 1);
        X509V3_CTX extensionContext{};
        X509V3_set_ctx(&extensionContext, cert.get(), cert.get(), nullptr, nullptr, 0);
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
            X509V3_EXT_conf_nid(nullptr, &extensionContext, NID_subject_alt_name,
                const_cast<char*>("DNS:server.test,IP:127.0.0.1,IP:::1")), X509_EXTENSION_free);
        ASSERT_NE(san, nullptr);
        ASSERT_EQ(X509_add_ext(cert.get(), san.get(), -1), 1);
        ASSERT_GT(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
        ASSERT_EQ(SSL_CTX_use_certificate(m_server.m_ctx, cert.get()), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey(m_server.m_ctx, key.get()), 1);
        std::unique_ptr<BIO, decltype(&BIO_free)> pem(BIO_new(BIO_s_mem()), BIO_free);
        ASSERT_NE(pem, nullptr);
        ASSERT_EQ(PEM_write_bio_X509(pem.get(), cert.get()), 1);
        char* bytes = nullptr;
        long length = BIO_get_mem_data(pem.get(), &bytes);
        ASSERT_GT(length, 0);
        m_certificate.assign(bytes, static_cast<size_t>(length));
    }

    void MakeConnections(bool verify, bool trust)
    {
        if (verify)
        {
            m_client.EnablePeerVerification();
        }
        if (trust)
        {
            ASSERT_TRUE(m_client.AddTrustedCertificate(m_certificate.data(), m_certificate.size()));
        }
        m_serverConnection = std::make_unique<ssl::Connection>(m_server, m_descriptor,
            m_serverBuffer, sizeof(m_serverBuffer));
        m_clientConnection = std::make_unique<ssl::Connection>(m_client, m_descriptor,
            m_clientBuffer, sizeof(m_clientBuffer));
    }

    static bool Transfer(SSL* from, SSL* to)
    {
        char buffer[4096];
        int count;
        while ((count = BIO_read(SSL_get_wbio(from), buffer, sizeof(buffer))) > 0)
        {
            if (BIO_write(SSL_get_rbio(to), buffer, count) != count)
            {
                return false;
            }
        }
        return true;
    }

    bool Handshake()
    {
        SSL* client = m_clientConnection->m_ssl;
        SSL* server = m_serverConnection->m_ssl;
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            for (SSL* session : {client, server})
            {
                ERR_clear_error();
                int result = SSL_do_handshake(session);
                int error = SSL_get_error(session, result);
                if (result != 1 && error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                {
                    return false;
                }
                if (!Transfer(session, session == client ? server : client))
                {
                    return false;
                }
            }
            if (SSL_is_init_finished(client) && SSL_is_init_finished(server))
            {
                return true;
            }
        }
        ADD_FAILURE() << "Memory BIO handshake failed to converge";
        return false;
    }
};

TEST_F(TlsClientConfigTest, TrustedDnsIdentityAndSni)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, true));
    // OpenSSL owns its copy: the original caller strings need not survive the handshake.
    //
    std::string host = "server.test";
    ASSERT_TRUE(m_clientConnection->SetServerName(host.c_str()));
    ASSERT_TRUE(m_clientConnection->SetVerifyHost(host.c_str()));
    host.assign(host.size(), 'x');
    ASSERT_TRUE(Handshake());
    EXPECT_EQ(SSL_get_verify_result(m_clientConnection->m_ssl), X509_V_OK);
    EXPECT_STREQ(SSL_get_servername(m_serverConnection->m_ssl, TLSEXT_NAMETYPE_host_name),
        "server.test");
}

TEST_F(TlsClientConfigTest, TrustedWrongDnsIdentityIsRejected)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, true));
    ASSERT_TRUE(m_clientConnection->SetServerName("server.test"));
    ASSERT_TRUE(m_clientConnection->SetVerifyHost("wrong.test"));
    EXPECT_FALSE(Handshake());
    EXPECT_EQ(SSL_get_verify_result(m_clientConnection->m_ssl), X509_V_ERR_HOSTNAME_MISMATCH);
}

TEST_F(TlsClientConfigTest, UntrustedCorrectDnsIdentityIsRejected)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, false));
    ASSERT_TRUE(m_clientConnection->SetVerifyHost("server.test"));
    EXPECT_FALSE(Handshake());
    EXPECT_NE(SSL_get_verify_result(m_clientConnection->m_ssl), X509_V_OK);
}

TEST_F(TlsClientConfigTest, VerificationRemainsExplicit)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(false, false));
    EXPECT_EQ(SSL_CTX_get_verify_mode(m_client.m_ctx), SSL_VERIFY_NONE);
    ASSERT_TRUE(m_clientConnection->SetVerifyHost("wrong.test"));
    ASSERT_TRUE(m_clientConnection->SetServerName("server.test"));
    EXPECT_EQ(SSL_get_verify_mode(m_clientConnection->m_ssl), SSL_VERIFY_NONE);
    EXPECT_TRUE(Handshake());
}

TEST_F(TlsClientConfigTest, TrustedIpv4IdentityWithoutSni)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, true));
    ASSERT_TRUE(m_clientConnection->SetVerifyIp("127.0.0.1"));
    ASSERT_TRUE(Handshake());
    EXPECT_EQ(SSL_get_verify_result(m_clientConnection->m_ssl), X509_V_OK);
    EXPECT_EQ(SSL_get_servername(m_serverConnection->m_ssl, TLSEXT_NAMETYPE_host_name), nullptr);
}

TEST_F(TlsClientConfigTest, TrustedIpv6IdentityWithoutSni)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, true));
    ASSERT_TRUE(m_clientConnection->SetVerifyIp("::1"));
    EXPECT_TRUE(Handshake());
    EXPECT_EQ(SSL_get_verify_result(m_clientConnection->m_ssl), X509_V_OK);
}

TEST_F(TlsClientConfigTest, TrustedWrongIpIdentityIsRejected)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, true));
    ASSERT_TRUE(m_clientConnection->SetVerifyIp("127.0.0.2"));
    EXPECT_FALSE(Handshake());
    EXPECT_EQ(SSL_get_verify_result(m_clientConnection->m_ssl), X509_V_ERR_IP_ADDRESS_MISMATCH);
}

TEST_F(TlsClientConfigTest, InvalidInputsPreserveExistingDnsIdentity)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, true));
    ASSERT_TRUE(m_clientConnection->SetServerName("server.test"));
    ASSERT_TRUE(m_clientConnection->SetVerifyHost("server.test"));
    EXPECT_FALSE(m_clientConnection->SetServerName(nullptr));
    EXPECT_FALSE(m_clientConnection->SetServerName(""));
    EXPECT_FALSE(m_clientConnection->SetVerifyHost(nullptr));
    EXPECT_FALSE(m_clientConnection->SetVerifyHost(""));
    EXPECT_STREQ(X509_VERIFY_PARAM_get0_host(SSL_get0_param(m_clientConnection->m_ssl), 0),
        "server.test");
    EXPECT_TRUE(Handshake());
}

TEST_F(TlsClientConfigTest, InvalidInputsPreserveExistingIpIdentity)
{
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, true));
    ASSERT_TRUE(m_clientConnection->SetVerifyIp("127.0.0.1"));
    for (const char* invalid : {"", "server.test", "[::1]", "127.0.0.1:443", "::1%lo"})
    {
        EXPECT_FALSE(m_clientConnection->SetVerifyIp(invalid));
    }
    EXPECT_FALSE(m_clientConnection->SetVerifyIp(nullptr));
    EXPECT_TRUE(Handshake());
}

TEST_F(TlsClientConfigTest, InvalidTrustInputDoesNotAddAnAnchor)
{
    EXPECT_FALSE(m_client.AddTrustedCertificate(nullptr, 10));
    EXPECT_FALSE(m_client.AddTrustedCertificate("", 0));
    EXPECT_FALSE(m_client.AddTrustedCertificate("x", static_cast<size_t>(INT_MAX) + 1));
    EXPECT_FALSE(m_client.AddTrustedCertificate("garbage", 7));
    std::string bundle = m_certificate + m_certificate;
    EXPECT_FALSE(m_client.AddTrustedCertificate(bundle.data(), bundle.size()));
    std::string garbage = m_certificate + "garbage";
    EXPECT_FALSE(m_client.AddTrustedCertificate(garbage.data(), garbage.size()));
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, false));
    ASSERT_TRUE(m_clientConnection->SetVerifyHost("server.test"));
    EXPECT_FALSE(Handshake());
    EXPECT_NE(SSL_get_verify_result(m_clientConnection->m_ssl), X509_V_OK);
}

TEST_F(TlsClientConfigTest, TrustInputCanBeReleasedAndDoesNotEnableVerification)
{
    std::string certificate = m_certificate + " \t\n\r";
    ASSERT_TRUE(m_client.AddTrustedCertificate(certificate.data(), certificate.size()));
    certificate.assign(certificate.size(), 'x');
    EXPECT_EQ(SSL_CTX_get_verify_mode(m_client.m_ctx), SSL_VERIFY_NONE);
    ASSERT_NO_FATAL_FAILURE(MakeConnections(true, false));
    ASSERT_TRUE(m_clientConnection->SetVerifyHost("server.test"));
    EXPECT_TRUE(Handshake());
}

} // namespace
