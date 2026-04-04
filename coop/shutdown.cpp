#include "shutdown.h"

#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

#include "cooperator.h"

namespace coop
{

static int g_shutdownFd = -1;

static void SignalHandler(int)
{
    // write() is async-signal-safe
    //
    uint64_t one = 1;
    [[maybe_unused]] ssize_t n = write(g_shutdownFd, &one, sizeof(one));
}

static void ShutdownWatcher()
{
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

    struct sigaction sa{};
    sa.sa_handler = SignalHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    int ret = sigaction(SIGINT, &sa, nullptr);
    assert(ret == 0);
    ret = sigaction(SIGTERM, &sa, nullptr);
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
