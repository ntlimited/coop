#pragma once

// ws::Upgrade — WebSocket handshake validation and 101 response.
//
// Two-step upgrade pattern:
//
//   1. ws::Upgrade(conn) validates headers and sends 101 Switching Protocols
//   2. Handler constructs ws::Connection<Transport> from the bump heap
//
// Usage:
//
//     void HandleWs(http::ConnectionBase& conn) {
//         if (!ws::Upgrade(conn)) return;
//
//         http::PlaintextTransport transport(conn.GetDescriptor());
//         auto ws = coop::Allocate<ws::Connection<http::PlaintextTransport>>(
//             ws::Connection<http::PlaintextTransport>::ExtraBytes(),
//             transport, coop::Self(),
//             ws::ConnectionBase::DEFAULT_RECV_BUFFER_SIZE,
//             ws::ConnectionBase::DEFAULT_SEND_BUFFER_SIZE,
//             std::chrono::seconds(30),
//             conn.LeftoverData(), conn.LeftoverSize());
//
//         while (auto* frame = ws->NextFrame()) { ... }
//         ws->Close(1000);
//     }
//

#include "coop/http/connection.h"

namespace coop
{
namespace ws
{

// Validate WebSocket upgrade headers and send the 101 Switching Protocols response.
// Returns true on success (protocol is now WebSocket). Returns false on validation failure
// (sends 400 to the client).
//
// After a successful return, the handler should construct a ws::Connection<Transport>
// using conn.LeftoverData()/LeftoverSize() to capture any bytes already in the HTTP
// recv buffer.
//
bool Upgrade(http::ConnectionBase& conn);

} // namespace coop::ws
} // namespace coop
