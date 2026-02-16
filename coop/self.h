#pragma once


namespace coop
{

struct Context;
struct Cooperator;

namespace io { struct Uring; }

// This lets code use thread locals to get the context that is running instead of passing it around.
// I know, thread locals, right? Ultimately, the register for them exists whether we like it or not,
// the state is being tracked already, and the thread local hookup is only done when the cooperator
// starts.
//
Context* Self();

bool Yield();

bool IsKilled();

bool IsShuttingDown();

// Convenience accessors that go straight through the thread-local cooperator, avoiding the
// Self() -> Context -> Cooperator round-trip.
//
Cooperator* GetCooperator();

io::Uring* GetUring();

} // end namespace coop
