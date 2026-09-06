#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace coop
{

struct Context;
struct Cooperator;

namespace debug
{

// Context introspection — the stackful superpower.
//
// A cooperator parks a suspended context by saving its stack pointer; the whole call chain it was in
// the middle of is still sitting on its stack. So unlike a stackless coroutine or an async/await
// future (where a blocked task is an opaque state-machine cell with no recoverable call stack), coop
// can walk any parked context and tell you exactly where it is stuck and how it got there. This is
// what turns "the server is wedged" into "context 'upload-42' is blocked in Coordinator::Acquire,
// called from S3Put, called from HandleRequest."
//
// Requires -fno-omit-frame-pointer (coop is built that way; user frames unwind as far as they keep
// frame pointers too). Symbolization needs the binary linked with -rdynamic (or symbols otherwise
// visible to dladdr); without it, frames come back as lib+offset.

// Walk `ctx`'s call stack into `frames` (most-recent first), returning the number captured. For a
// suspended context this reconstructs the stack from its saved stack pointer; for the currently
// running context it walks the live frame. Safe to call from any context on `ctx`'s cooperator.
//
int CaptureStack(Context* ctx, uintptr_t* frames, int maxFrames);

// Best-effort symbol for an instruction address: a demangled C++ name if dladdr can resolve one,
// else "shared-object+0xoffset", else "0xaddr". Writes a NUL-terminated string; returns its length.
//
size_t Symbolize(uintptr_t pc, char* buf, size_t bufSize);

// Dump every context on `co` — name, scheduler state, and a symbolized backtrace — to `out` (or the
// spdlog warn stream). Call only from a context on this cooperator, e.g. its status endpoint.
// This allocates, symbolizes, and writes output: it is NOT async-signal-safe and cannot run from
// a watchdog hook on another thread. A signal handler must arrange a deferred request through an
// async-signal-safe mechanism; a pinned cooperator needs the stall capture or an external debugger.
//
void DumpContexts(Cooperator* co, FILE* out);
void DumpContexts(Cooperator* co);

} // end namespace coop::debug
} // end namespace coop
