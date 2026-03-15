#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{
namespace ws
{

enum class Opcode : uint8_t
{
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

// Flyweight frame descriptor returned by NextFrame(). Points into the recv buffer — valid
// until the next call to NextFrame() or any buffer-compacting operation.
//
// For continuation frames, opcode is set to the original message opcode (Text/Binary),
// not Continuation — the handler sees the logical message type. Control frames (Ping/Pong/
// Close) are delivered as-is even when interleaved with data frame continuations.
//
struct Frame
{
    Opcode      opcode;
    bool        fin;          // FIN bit — true if this is the last frame of the message
    const void* data;         // payload pointer (into recv buffer, unmasked in-place)
    size_t      size;         // payload bytes available in this chunk
    bool        complete;     // true if all payload bytes for this frame have been delivered

    bool IsClose()  const { return opcode == Opcode::Close; }
    bool IsPing()   const { return opcode == Opcode::Ping; }
    bool IsPong()   const { return opcode == Opcode::Pong; }
    bool IsText()   const { return opcode == Opcode::Text; }
    bool IsBinary() const { return opcode == Opcode::Binary; }
};

} // namespace coop::ws
} // namespace coop
