#pragma once

#include <cstddef>

#include "coop/coordinator.h"
#include "coop/detail/embedded_list.h"
#include "coop/time/interval.h"

namespace coop
{

struct Context;

namespace io
{
struct Descriptor;
}

namespace http
{

// ServerHandle: graceful-drain control for a RunServer / RunTlsServer, passed by pointer
// in ServerConfiguration::control. Without one, the server behaves exactly as before and
// pays nothing — every drain check is gated on the pointer being present.
//
// Drain(timeout) is the two-phase graceful shutdown every peer runtime makes users
// hand-build (Go's Server.Shutdown, Envoy's drain):
//
//   Soft — stop accepting (shut the listen socket so the accept loop breaks), then wake
//   idle keep-alive connections by shutting their read side. An idle connection blocked
//   for its next request sees EOF and exits cleanly; a connection mid-response finishes
//   with Connection: close when headers have not yet been sent, then exits. A partial
//   request line is idle; an active handler may finish reading headers and body. TLS
//   handshakes remain live until they complete or the hard deadline arrives.
//
//   Hard — connections still live at the deadline get their sockets fully shut
//   (SHUT_RDWR), waking socket IO so handlers can unwind. Drain waits for transport and
//   descriptor teardown before returning. A handler blocked on unrelated work must
//   arrange its own cancellation: timeout bounds the soft phase, not total return time.
//
// In drain mode each connection DETACHES from the acceptor at launch and registers here,
// so stopping the acceptor does not cascade-kill live connections. The caller must keep
// ServerHandle alive until RunServer / RunTlsServer returns and all connections exit;
// handler state must outlive the connections. Drain waits for the connections, but the
// acceptor may still be unwinding when it returns. Single-cooperator: server, connections
// and Drain caller all live on one cooperator; no atomics.
//
struct ConnectionBase;

struct ServerHandle
{
    // A live connection's registration node — a member of the connection handler,
    // stack-resident for the connection's lifetime.
    //
    struct ConnNode : EmbeddedListHookups<ConnNode>
    {
        io::Descriptor* desc = nullptr;
        ConnectionBase* connection = nullptr;
        bool idle = false;  // true only while awaiting the next HTTP request line
    };

    ServerHandle() = default;
    ServerHandle(ServerHandle const&) = delete;
    ServerHandle& operator=(ServerHandle const&) = delete;

    bool IsDraining() const { return m_draining; }

    // The listener is borrowed only while RunServer / RunTlsServer is active.
    //
    void SetListener(io::Descriptor* listener) { m_listener = listener; }
    void ClearListener(io::Descriptor* listener)
    {
        if (m_listener == listener) m_listener = nullptr;
    }
    bool HasListener() const { return m_listener != nullptr; }

    // Connection lifecycle (called from the connection handler).
    //
    void Register(ConnNode* node, io::Descriptor* desc)
    {
        node->desc = desc;
        m_conns.Push(node);
        m_liveCount++;
    }
    void Deregister(ConnNode* node)
    {
        m_conns.Remove(node);
        m_liveCount--;
        if (m_liveCount == 0 && m_drainWait.IsHeld())
        {
            m_drainWait.Release(nullptr, false);
        }
    }

    size_t LiveConnections() const { return m_liveCount; }

    // Graceful drain: soft phase, then a hard phase at `timeout`. Returns true if all
    // connections drained cleanly within the soft window (no hard kill needed).
    // Calls must be serialized. A subsequent call repeats the shutdown fan-out harmlessly.
    //
    bool Drain(Context* ctx, time::Interval timeout);

  private:
    void ShutdownAllConns(int how);

    bool                  m_draining = false;
    io::Descriptor*       m_listener = nullptr;
    Coordinator           m_drainWait;   // held while draining with live conns; wakes at 0
    EmbeddedList<ConnNode> m_conns;
    size_t                m_liveCount = 0;
};

} // end namespace coop::http
} // end namespace coop
