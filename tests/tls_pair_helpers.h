#pragma once

#include <cassert>
#include <cerrno>
#include <memory>
#include <sys/socket.h>

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "coop/cooperator.h"
#include "coop/countdown_latch.h"
#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/ssl/connection.h"
#include "coop/io/ssl/context.h"
#include "coop/io/ssl/recv.h"
#include "coop/io/ssl/send.h"
#include "test_helpers.h"

namespace test
{

struct TlsPair
{
    int fds[2];
    coop::io::Descriptor client;
    coop::io::Descriptor server;
    coop::io::ssl::Context clientContext{coop::io::ssl::Mode::Client};
    coop::io::ssl::Context serverContext{coop::io::ssl::Mode::Server};
    char clientBuffer[16384];
    char serverBuffer[16384];
    std::unique_ptr<coop::io::ssl::Connection> clientTls;
    std::unique_ptr<coop::io::ssl::Connection> serverTls;

    explicit TlsPair(bool socketBio = false)
    : fds{-1, -1}
    , client(coop::io::borrowed, -1)
    , server(coop::io::borrowed, -1)
    {
        EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);
        client.m_fd = fds[0];
        client.m_owned = true;
        server.m_fd = fds[1];
        server.m_owned = true;
        int size = 4096;
        EXPECT_EQ(setsockopt(server.m_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
        MakeCertificate();
        if (socketBio)
        {
            clientTls = std::make_unique<coop::io::ssl::Connection>(
                clientContext, client, coop::io::ssl::SocketBio{});
            serverTls = std::make_unique<coop::io::ssl::Connection>(
                serverContext, server, coop::io::ssl::SocketBio{});
            coop::CountdownLatch done(1);
            coop::Spawn({.stackSize = 65536}, [&](coop::Context*)
            {
                EXPECT_EQ(serverTls->HandshakeKill(), 0);
                done.Arrive();
            });
            EXPECT_EQ(clientTls->HandshakeKill(), 0);
            EXPECT_EQ(done.WaitKill(coop::Self()),
                      coop::CountdownLatch::WaitStatus::Completed);
        }
        else
        {
            clientTls = std::make_unique<coop::io::ssl::Connection>(
                clientContext, client, clientBuffer, sizeof(clientBuffer));
            serverTls = std::make_unique<coop::io::ssl::Connection>(
                serverContext, server, serverBuffer, sizeof(serverBuffer));
            for (int step = 0; step < 100; ++step)
            {
                SSL_do_handshake(clientTls->m_ssl);
                Transfer(clientTls->m_ssl, serverTls->m_ssl);
                SSL_do_handshake(serverTls->m_ssl);
                Transfer(serverTls->m_ssl, clientTls->m_ssl);
                if (SSL_is_init_finished(clientTls->m_ssl)
                    && SSL_is_init_finished(serverTls->m_ssl)) break;
            }
            EXPECT_TRUE(SSL_is_init_finished(clientTls->m_ssl));
            EXPECT_TRUE(SSL_is_init_finished(serverTls->m_ssl));
        }
    }

    void MakeCertificate()
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        ASSERT_NE(generator, nullptr);
        ASSERT_EQ(EVP_PKEY_keygen_init(generator.get()), 1);
        ASSERT_EQ(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(),
                                                       NID_X9_62_prime256v1), 1);
        EVP_PKEY* raw = nullptr;
        ASSERT_EQ(EVP_PKEY_keygen(generator.get(), &raw), 1);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        ASSERT_NE(cert, nullptr);
        ASSERT_EQ(X509_set_version(cert.get(), 2), 1);
        ASSERT_EQ(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1), 1);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60), nullptr);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400), nullptr);
        ASSERT_EQ(X509_set_pubkey(cert.get(), key.get()), 1);
        auto* name = X509_get_subject_name(cert.get());
        ASSERT_EQ(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0), 1);
        ASSERT_EQ(X509_set_issuer_name(cert.get(), name), 1);
        ASSERT_GT(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
        ASSERT_EQ(SSL_CTX_use_certificate(serverContext.m_ctx, cert.get()), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey(serverContext.m_ctx, key.get()), 1);
        ASSERT_EQ(SSL_CTX_set_max_proto_version(serverContext.m_ctx, TLS1_2_VERSION), 1);
        ASSERT_EQ(SSL_CTX_set_max_proto_version(clientContext.m_ctx, TLS1_2_VERSION), 1);
    }

    static void Transfer(SSL* from, SSL* to)
    {
        char buffer[4096];
        int n;
        while ((n = BIO_read(SSL_get_wbio(from), buffer, sizeof(buffer))) > 0)
            EXPECT_EQ(BIO_write(SSL_get_rbio(to), buffer, n), n);
    }

    int Recv(void* data, size_t size)
    {
        return clientTls
            ? coop::io::ssl::RecvKill(*clientTls, data, size, std::chrono::milliseconds(150))
            : coop::io::RecvKill(client, data, size, 0, std::chrono::milliseconds(150));
    }

    void FillSocket()
    {
        char buffer[4096]{};
        size_t total = 0;
        while (total < 1024 * 1024)
        {
            ssize_t n = send(server.m_fd, buffer, sizeof(buffer), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0)
            {
                EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
                return;
            }
            ASSERT_GT(n, 0);
            total += static_cast<size_t>(n);
        }
        FAIL() << "socket did not fill within the test bound";
    }
};

} // namespace test
