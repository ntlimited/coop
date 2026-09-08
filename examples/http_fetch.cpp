// Fetch one resource with the native streaming HTTP/1.1 client. DNS names and IPv4
// addresses use coop's Connect helper; redirects and retries remain application policy.
//
//   http_fetch example.com 443 / --tls
//   http_fetch 127.0.0.1 8080 /
//
#include <arpa/inet.h>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <sys/socket.h>

#include "coop/alloc.h"
#include "coop/cooperator.h"
#include "coop/thread.h"
#include "coop/http/client.h"
#include "coop/http/transport.h"
#include "coop/http/tls_transport.h"
#include "coop/io/connect.h"
#include "coop/io/ssl/context.h"

using namespace coop;

template<typename Transport>
bool ReadResource(Context* ctx, Transport transport, const char* authority, const char* path)
{
    using Client = http::ClientConnection<Transport>;
    // The descriptor, TLS session (if any), and authority outlive this allocation.
    // Its receive/send buffers are trailing storage, allocated once for the connection.
    //
    auto client = ctx->Allocate<Client>(Client::ExtraBytes(), transport, authority);
    if (!client->Get(path))
    {
        fprintf(stderr, "request failed: %d\n", client->Error());
        return false;
    }

    int status;
    while (true)
    {
        auto* line = client->GetResponseLine();
        if (!line)
        {
            fprintf(stderr, "response line failed: %d\n", client->Error());
            return false;
        }
        status = line->status; // retain the scalar before advancing borrowed views
        if (status >= 200) break;
        if (status == 101)
        {
            fprintf(stderr, "unexpected protocol upgrade\n");
            return false;
        }
        // Informational responses are observable. This GET example explicitly advances
        // past them; an Expect: 100-continue upload could choose to send its body here.
        //
        if (!client->SkipBody() || !client->AdvanceResponse()) return false;
    }

    uint64_t bytes = 0;
    while (true)
    {
        auto body = client->NextBody();
        if (!body)
        {
            if (!body.Complete())
            {
                fprintf(stderr, "response body failed after %llu bytes: %d\n",
                    static_cast<unsigned long long>(bytes), body.Error());
                return false;
            }
            break;
        }
        // Process body->data here, before the next parsing operation. Counting consumes
        // the body with no accumulated string, callback, or per-chunk allocation.
        //
        bytes += body->size;
    }
    printf("HTTP %d  %llu bytes  reusable=%s\n", status,
        static_cast<unsigned long long>(bytes), client->Reusable() ? "yes" : "no");
    // This example closes after one response. A caller retaining the connection can
    // explicitly Reset() after Reusable(), then issue its next request.
    //
    return true;
}

bool Fetch(Context* ctx, const char* host, int port, const char* authority,
           const char* path, io::ssl::Context* tlsContext)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        perror("socket");
        return false;
    }
    io::Descriptor descriptor(fd);
    int result = io::Connect(descriptor, host, port);
    if (result < 0)
    {
        fprintf(stderr, "connect failed: %d\n", result);
        return false;
    }
    if (!tlsContext)
    {
        return ReadResource(ctx,
            http::PlaintextTransport(descriptor, http::RecvPolicy::PollFirst), authority, path);
    }

    io::ssl::Connection tls(*tlsContext, descriptor, io::ssl::SocketBio{});
    in_addr address;
    bool identitySet = inet_pton(AF_INET, host, &address) == 1
        ? tls.SetVerifyIp(host)
        : tls.SetServerName(host) && tls.SetVerifyHost(host);
    if (!identitySet)
    {
        fprintf(stderr, "TLS identity configuration failed\n");
        return false;
    }
    result = tls.Handshake();
    if (result < 0)
    {
        fprintf(stderr, "TLS handshake failed: %d (verification=%ld)\n",
            result, SSL_get_verify_result(tls.m_ssl));
        return false;
    }
    return ReadResource(ctx, http::TlsTransport(tls, descriptor), authority, path);
}

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 5 || (argc == 5 && strcmp(argv[4], "--tls") != 0))
    {
        fprintf(stderr, "usage: http_fetch HOST PORT /PATH [--tls]\n");
        return 2;
    }
    int port = 0;
    const char* portEnd = argv[2] + strlen(argv[2]);
    auto parsed = std::from_chars(argv[2], portEnd, port);
    if (parsed.ec != std::errc{} || parsed.ptr != portEnd || port < 1 || port > 65535)
    {
        fprintf(stderr, "invalid port\n");
        return 2;
    }

    // Trust loading is an explicit setup operation, outside the cooperative thread.
    // Applications with memory-resident anchors can use AddTrustedCertificate instead.
    //
    std::optional<io::ssl::Context> tls;
    if (argc == 5)
    {
        tls.emplace(io::ssl::Mode::Client);
        tls->EnablePeerVerification();
        if (!tls->LoadDefaultVerifyPaths())
        {
            fprintf(stderr, "TLS trust setup failed\n");
            return 1;
        }
    }
    const std::string authority = std::string(argv[1]) + ":" + argv[2];
    bool success = false;
    Cooperator co;
    Thread thread(&co);
    bool submitted = co.SubmitSync([&](Context* ctx)
    {
        success = Fetch(ctx, argv[1], port, authority.c_str(), argv[3], tls ? &*tls : nullptr);
        co.Shutdown();
    });
    return submitted && success ? 0 : 1;
}
