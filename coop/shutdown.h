#pragma once

namespace coop
{

// Install SIGINT/SIGTERM handlers and start a background watcher thread. When SIGINT or SIGTERM
// arrives, the watcher calls Cooperator::ShutdownAll() to shut down every live cooperator.
//
// Call once before starting any cooperator.
//
void InstallShutdownHandler();

} // end namespace coop
