#include "shutdown.h"

#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

#include "cooperator.h"
#include "signal_stack.h"

namespace coop
{

static int g_shutdownFd = -1;

// Runs on the alternate stack of whichever thread the kernel picks — including a cooperator thread
// mid-context, which is why the registration below asks for SA_ONSTACK.
//
static void SignalHandler(int, siginfo_t*, void*)
{
    // write() is async-signal-safe
    //
    uint64_t one = 1;
    [[maybe_unused]] ssize_t n = write(g_shutdownFd, &one, sizeof(one));
}

static void ShutdownWatcher()
{
    // This thread can be the one the kernel picks for a process-directed SIGINT, so it needs an
    // alternate stack of its own. It is not running contexts, but the handler below is registered
    // process-wide with SA_ONSTACK and a thread without an alternate stack quietly ignores the flag.
    //
    SignalStack signalStack;

    uint64_t count = 0;
    while (read(g_shutdownFd, &count, sizeof(count)) < 0 && errno == EINTR)
    {
    }
    Cooperator::ShutdownAll();
}

void InstallShutdownHandler()
{
    if (g_shutdownFd >= 0)
    {
        return;
    }

    g_shutdownFd = eventfd(0, EFD_CLOEXEC);
    assert(g_shutdownFd >= 0);

    // The calling thread is usually the host's main thread, which outlives every cooperator and can
    // be handed a process-directed SIGINT at any moment. Its alternate stack has to outlive this
    // call, so it is a static; InstallShutdownHandler is single-entry (the eventfd check above), so
    // it is initialized exactly once.
    //
    static SignalStack mainThreadSignalStack;
    (void)mainThreadSignalStack;

    int ret = RegisterOnAltStack(SIGINT, SignalHandler, SA_RESTART);
    assert(ret == 0);
    ret = RegisterOnAltStack(SIGTERM, SignalHandler, SA_RESTART);
    assert(ret == 0);

    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ret = sigaction(SIGPIPE, &ignore, nullptr);
    assert(ret == 0);

    std::thread watcher(ShutdownWatcher);
    watcher.detach();
}

} // end namespace coop
