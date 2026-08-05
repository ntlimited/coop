#pragma once

// curl-multi under coop — driving libcurl's multi interface from an io_uring cooperator.
//
// libcurl's multi API is built for a foreign event loop: it hands you sockets to watch (via
// CURLMOPT_SOCKETFUNCTION) and a single timeout to honor (CURLMOPT_TIMERFUNCTION), and you call
// curl_multi_socket_action whenever one of those fires. That interface is *readiness*-shaped
// (epoll-style "tell me when fd X is writable"); io_uring is *completion*-shaped. The bridge is
// one io_uring POLL_ADD per socket, wrapped as a coop context that blocks on readiness — coop's
// io::Poll. curl owns every byte of protocol (HTTP/1.1, HTTP/2, ...); coop is pure substrate.
//
// This is a demo/example driver: one CURLM on one cooperator (curl-multi is not thread-safe, and a
// cooperator is single-threaded, so every curl call here is naturally serialized). It leans on one
// property of the socket-action model: curl drops a socket from the active set the moment its
// transfer finishes (CURL_POLL_REMOVE), so a per-socket watcher is transient and self-terminating —
// no cross-context kill, no Handle bookkeeping. A driver that had to survive cross-socket interest
// changes (curl re-arming socket A because of activity on B) would upgrade the watcher to a
// select over (poll, interest-changed signal); that case does not arise for independent transfers.

#include <curl/curl.h>
#include <poll.h>

#include <memory>
#include <unordered_map>

#include <cstdio>
#include <cstdlib>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/coordinate_with.h"
#include "coop/self.h"
#include "coop/io/descriptor.h"
#include "coop/io/poll.h"
#include "coop/time/sleep.h"

#ifndef CURLCOOP_TRACE
#define CURLCOOP_TRACE 0
#endif
#define CURLCOOP_LOG(...) do { if (CURLCOOP_TRACE) fprintf(stderr, "[curlcoop] " __VA_ARGS__); } while (0)

namespace curlcoop
{

using namespace coop;

// Per-transfer completion latch. The caller of Perform sets this as the easy handle's private
// pointer, then blocks on `done`; the driver's CheckInfo fires it when curl reports the transfer
// finished. Signal is one-shot and already-signaled-safe, so there is no missed-wake race even if
// the transfer somehow completes before the caller waits.
//
struct Transfer
{
    Coordinator done;          // parked by the caller, released by CheckInfo (semaphore idiom)
    CURLcode    result   = CURLE_OK;
    long        status   = 0;
    bool        finished = false;
};

class Driver
{
public:
    explicit Driver(Cooperator* co) : m_co(co)
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

    // Run one easy handle to completion, blocking `ctx` (but not the thread). Adds the handle to
    // the multi; curl's timer callback kicks the first socket_action, watcher contexts drive the
    // IO, and CheckInfo wakes us on CURLMSG_DONE. Returns the transfer's CURLcode; *statusOut, if
    // given, gets the HTTP response code.
    //
    CURLcode Perform(Context* ctx, CURL* easy, long* statusOut = nullptr)
    {
        Transfer t;
        curl_easy_setopt(easy, CURLOPT_PRIVATE, reinterpret_cast<char*>(&t));
        t.done.TryAcquire(ctx);                       // park latch (held until CheckInfo releases)
        curl_multi_add_handle(m_multi, easy);
        CURLCOOP_LOG("added handle %p\n", (void*)easy);

        if (!t.finished)
        {
            CoordinateWith(ctx, &t.done);             // block until the transfer completes
        }
        if (statusOut) *statusOut = t.status;
        return t.result;
    }

private:
    struct Sock
    {
        curl_socket_t fd;
        unsigned      mask;      // poll(2) events curl currently wants
        bool          removed = false;
    };

    // ---- curl callbacks (static thunks → member handlers) ----

    static int SocketCb(CURL*, curl_socket_t s, int what, void* userp, void*)
    {
        static_cast<Driver*>(userp)->OnSocket(s, what);
        return 0;
    }

    static int TimerCb(CURLM*, long timeoutMs, void* userp)
    {
        static_cast<Driver*>(userp)->OnTimer(timeoutMs);
        return 0;
    }

    void OnSocket(curl_socket_t s, int what)
    {
        CURLCOOP_LOG("OnSocket fd=%d what=%d\n", (int)s, what);
        if (what == CURL_POLL_REMOVE)
        {
            auto it = m_socks.find(s);
            if (it != m_socks.end())
            {
                it->second->removed = true;   // its watcher sees this and self-exits + erases
            }
            return;
        }

        unsigned mask = 0;
        if (what == CURL_POLL_IN)         mask = POLLIN;
        else if (what == CURL_POLL_OUT)   mask = POLLOUT;
        else if (what == CURL_POLL_INOUT) mask = POLLIN | POLLOUT;

        auto it = m_socks.find(s);
        if (it == m_socks.end())
        {
            auto st  = std::make_unique<Sock>(Sock{s, mask, false});
            Sock* raw = st.get();
            m_socks.emplace(s, std::move(st));
            SpawnWatcher(raw);
        }
        else
        {
            // Same socket, new interest. The watcher re-reads mask each loop; changes here are
            // triggered by this socket's own activity (inside its watcher's socket_action call),
            // so it re-arms with the new mask on its very next iteration.
            //
            it->second->mask = mask;
        }
    }

    void SpawnWatcher(Sock* st)
    {
        m_co->Spawn([this, st](Context* c)
        {
            c->SetName("curl-sock");
            c->Detach();

            // Defer off the curl call stack. Eager Spawn runs us synchronously inside the
            // curl_multi_socket_action that registered this socket; io::Poll can even complete
            // inline for an already-ready loopback fd, which would drive socket_action reentrantly
            // (CURLM_RECURSIVE_API_CALL). Yield once so our socket_action calls only ever originate
            // from the scheduler loop, never nested in another curl_multi call.
            //
            c->Yield(true);

            io::Descriptor desc(io::borrowed, st->fd);   // curl owns the fd — do not close it

            while (!st->removed)
            {
                unsigned mask = st->mask;
                int rev = io::Poll(desc, mask);
                if (st->removed) break;

                int ev = 0;
                if (rev < 0)
                {
                    ev = CURL_CSELECT_ERR;
                }
                else
                {
                    if (rev & (POLLIN | POLLHUP | POLLERR)) ev |= CURL_CSELECT_IN;
                    if (rev & POLLOUT)                      ev |= CURL_CSELECT_OUT;
                }

                int running = 0;
                CURLMcode mc = curl_multi_socket_action(m_multi, st->fd, ev, &running);
                CURLCOOP_LOG("watcher fd=%d rev=0x%x ev=0x%x -> mc=%d running=%d mask=0x%x\n",
                             (int)st->fd, rev, ev, mc, running, st->mask);
                CheckInfo();
                // Loop condition re-checks st->removed: a transfer that just finished (or whose
                // socket curl dropped) set it inside the socket_action above.
            }

            m_socks.erase(st->fd);   // frees the Sock; st dangles after this — do not touch
        });
    }

    void OnTimer(long timeoutMs)
    {
        long gen = ++m_timerGen;          // supersede any pending timer
        if (timeoutMs < 0) return;        // curl wants no timer

        m_co->Spawn([this, gen, timeoutMs](Context* c)
        {
            c->SetName("curl-timer");
            c->Detach();

            // Defer off the curl call stack: curl-multi is not reentrant, and eager Spawn runs us
            // synchronously inside the curl_multi_* call that armed this timer. Sleeping (or, for a
            // 0ms timer, a bare yield) returns control to that call first; we drive socket_action
            // only once it has returned.
            //
            if (timeoutMs > 0)
            {
                time::Sleep(c, std::chrono::milliseconds(timeoutMs));
            }
            else
            {
                c->Yield(true);
            }
            if (gen != m_timerGen) return;   // a newer timeout replaced us

            CURLCOOP_LOG("timer fire ms=%ld gen=%ld\n", timeoutMs, gen);
            int running = 0;
            curl_multi_socket_action(m_multi, CURL_SOCKET_TIMEOUT, 0, &running);
            CheckInfo();
        });
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
                CURLCOOP_LOG("DONE easy=%p status=%ld res=%d\n", (void*)easy, t->status, res);
                t->done.Release(nullptr, false);   // wake the context blocked in Perform
            }
        }
    }

    Cooperator* m_co;
    CURLM*      m_multi;
    std::unordered_map<curl_socket_t, std::unique_ptr<Sock>> m_socks;
    long        m_timerGen = 0;
};

} // namespace curlcoop
