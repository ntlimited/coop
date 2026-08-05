#pragma once

// curl-multi under coop — driving libcurl's multi interface from an io_uring cooperator.
//
// libcurl's multi API is built for a foreign event loop: it hands you sockets to watch (via
// CURLMOPT_SOCKETFUNCTION) and a single timeout to honor (CURLMOPT_TIMERFUNCTION), and you call
// curl_multi_socket_action whenever one of those fires. That is exactly the shape coop::io::Reactor
// bridges — so this whole driver is a thin adapter: curl's socket/timer callbacks forward to the
// Reactor, and the Reactor's readiness/timeout callbacks call back into curl. curl owns every byte
// of protocol (HTTP/1.1, HTTP/2, ...); coop is pure substrate. HTTP/2 comes for free against a
// server that offers it — coop implements none of it.
//
// One CURLM per cooperator: curl-multi is not thread-safe, and a cooperator is single-threaded, so
// every curl call here is naturally serialized. The Reactor guarantees our socket_action calls
// never nest inside a curl callback (the CURLM_RECURSIVE_API_CALL trap), so this file carries no
// event-loop machinery of its own.
//
// ---------------------------------------------------------------------------------------------
// Benchmark context (benchmarks/bench_curl.cpp; loopback, 64 keep-alive connections, HTTP/1.1):
//
//     coop native client   ~180k req/s
//     curl via coop         ~79k req/s   (~2.3x slower)
//
// The gap is expected and worth understanding. coop's native client is a tight zero-copy pull
// parser on a persistent socket; curl carries a full protocol state machine plus this bridge's
// per-edge overhead (a poll SQE per readiness change, extra context hops). So: reach for the native
// client on the hot path of a protocol coop speaks. Reach for curl when you want what curl *is* —
// HTTP/2 and HTTP/3, proxies, redirects, cookies, auth schemes, content decoding, a mountain of
// battle-tested edge cases — none of which coop has to own. The ~2.3x buys all of that, unmodified.
// ---------------------------------------------------------------------------------------------

#include <curl/curl.h>
#include <poll.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/coordinate_with.h"
#include "coop/io/reactor.h"

namespace curlcoop
{

using namespace coop;

// Per-transfer completion latch. Perform sets this as the easy handle's private pointer and parks
// on `done`; CheckInfo releases it when curl reports the transfer finished. This is the semaphore
// park/wake idiom — a stack Coordinator held by the waiter, released by another context.
//
struct Transfer
{
    Coordinator done;
    CURLcode    result   = CURLE_OK;
    long        status   = 0;
    bool        finished = false;
};

class Driver
{
public:
    explicit Driver(Cooperator* co)
        : m_reactor(co, &Driver::OnReady, &Driver::OnTimeout, this)
    {
        m_multi = curl_multi_init();
        curl_multi_setopt(m_multi, CURLMOPT_SOCKETFUNCTION, &Driver::SocketCb);
        curl_multi_setopt(m_multi, CURLMOPT_SOCKETDATA, this);
        curl_multi_setopt(m_multi, CURLMOPT_TIMERFUNCTION, &Driver::TimerCb);
        curl_multi_setopt(m_multi, CURLMOPT_TIMERDATA, this);
    }

    ~Driver() { curl_multi_cleanup(m_multi); }

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    // Run one easy handle to completion, blocking `ctx` (but not the thread). curl's timer callback
    // kicks the first socket_action via the reactor; readiness callbacks drive the IO; CheckInfo
    // wakes us on CURLMSG_DONE. Returns the transfer's CURLcode; *statusOut, if given, gets the HTTP
    // response code.
    //
    CURLcode Perform(Context* ctx, CURL* easy, long* statusOut = nullptr)
    {
        Transfer t;
        curl_easy_setopt(easy, CURLOPT_PRIVATE, reinterpret_cast<char*>(&t));
        t.done.TryAcquire(ctx);
        curl_multi_add_handle(m_multi, easy);

        if (!t.finished)
        {
            CoordinateWith(ctx, &t.done);
        }
        if (statusOut) *statusOut = t.status;
        return t.result;
    }

private:
    // ---- curl → reactor ----

    static int SocketCb(CURL*, curl_socket_t s, int what, void* userp, void*)
    {
        auto* d = static_cast<Driver*>(userp);
        if (what == CURL_POLL_REMOVE)
        {
            d->m_reactor.Unwatch(static_cast<int>(s));
        }
        else
        {
            unsigned mask = 0;
            if (what == CURL_POLL_IN)         mask = POLLIN;
            else if (what == CURL_POLL_OUT)   mask = POLLOUT;
            else if (what == CURL_POLL_INOUT) mask = POLLIN | POLLOUT;
            d->m_reactor.Watch(static_cast<int>(s), mask);
        }
        return 0;
    }

    static int TimerCb(CURLM*, long timeoutMs, void* userp)
    {
        static_cast<Driver*>(userp)->m_reactor.SetTimeout(timeoutMs);
        return 0;
    }

    // ---- reactor → curl ----

    static void OnReady(int fd, unsigned revents, void* user)
    {
        int ev = 0;
        if (revents & (POLLIN | POLLHUP | POLLERR)) ev |= CURL_CSELECT_IN;
        if (revents & POLLOUT)                      ev |= CURL_CSELECT_OUT;

        auto* d = static_cast<Driver*>(user);
        int running = 0;
        curl_multi_socket_action(d->m_multi, fd, ev, &running);
        d->CheckInfo();
    }

    static void OnTimeout(void* user)
    {
        auto* d = static_cast<Driver*>(user);
        int running = 0;
        curl_multi_socket_action(d->m_multi, CURL_SOCKET_TIMEOUT, 0, &running);
        d->CheckInfo();
    }

    void CheckInfo()
    {
        CURLMsg* msg;
        int left;
        while ((msg = curl_multi_info_read(m_multi, &left)))
        {
            if (msg->msg != CURLMSG_DONE) continue;

            CURL* easy = msg->easy_handle;
            CURLcode res = msg->data.result;

            Transfer* t = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &t);

            curl_multi_remove_handle(m_multi, easy);

            if (t)
            {
                t->result = res;
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &t->status);
                t->finished = true;
                t->done.Release(nullptr, false);   // wake the context blocked in Perform
            }
        }
    }

    io::Reactor m_reactor;
    CURLM*      m_multi;
};

} // namespace curlcoop
