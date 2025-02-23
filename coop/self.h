#pragma once


namespace coop
{

struct Context;

// This lets code use thread locals to get the context that is running instead of passing it around.
// I know, thread locals, right? Ultimately, the register for them exists whether we like it or not,
// the state is being tracked already, and the thread local hookup is only done when the cooperator
// starts.
//
Context* Self();

bool Yield();

bool IsKilled();

} // end namespace coop
