#include "shutdown.h"

#include <cassert>
#include <csignal>
#include <thread>
#include <unistd.h>

#include "cooperator.h"

namespace coop
{

static int g_shutdownPipe[2];

static void SignalHandler(int)
{
    // write() is async-signal-safe
    //
    char x = 'x';
    write(g_shutdownPipe[1], &x, 1);
}

static void ShutdownWatcher()
{
    char buf;
    read(g_shutdownPipe[0], &buf, 1);
    Cooperator::ShutdownAll();
}

void InstallShutdownHandler()
{
    int ret = pipe(g_shutdownPipe);
    assert(ret == 0);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::thread watcher(ShutdownWatcher);
    watcher.detach();
}

} // end namespace coop
