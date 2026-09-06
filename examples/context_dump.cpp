// context_dump — a Go-goroutine-dump for coop, but with real C++ stacks for *blocked* contexts.
//
// Spawns a handful of named contexts parked on different things — a coordinator, a sleep, a nested
// call chain — then walks and prints every one. The point: each blocked context shows exactly where
// it is stuck and how it got there, recovered from its parked stack. Wire coop::debug::DumpContexts
// into a status endpoint on the owning cooperator. It is not async-signal-safe: signals must
// request a deferred dump, and a pinned cooperator needs the stall capture or an external debugger.
//
#include <cstdio>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/coordinator.h"
#include "coop/coordinate_with.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"
#include "coop/debug/introspect.h"

using namespace coop;

Coordinator* g_lock = nullptr;

[[gnu::noinline]] void AcquireTheLock(Context* c)
{
    CoordinateWith(c, g_lock);        // parks: waiting on a coordinator someone else holds
}

[[gnu::noinline]] void ProcessUpload(Context* c)
{
    AcquireTheLock(c);                // a nested call chain, so the dump shows the path
}

int main()
{
    Cooperator co(s_defaultCooperatorConfiguration);
    Thread thread(&co);

    co.SubmitSync([&](Context* ctx)
    {
        Coordinator lock;
        lock.TryAcquire(ctx);         // held for the life of the dump
        g_lock = &lock;

        co.Spawn([](Context* c) { c->SetName("upload-42"); ProcessUpload(c); });
        co.Spawn([](Context* c) { c->SetName("upload-43"); ProcessUpload(c); });
        co.Spawn([](Context* c)
        {
            c->SetName("timer-sweep");
            time::Sleep(c, std::chrono::seconds(3600));   // parked in a long sleep
        });

        for (int i = 0; i < 10; i++) ctx->Yield(true);    // let them all block

        debug::DumpContexts(&co, stdout);

        lock.Release(ctx);
        co.Shutdown();
    });

    return 0;
}
