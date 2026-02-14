#pragma once

#include <thread>

namespace coop
{

struct Cooperator;

// Helper for launching a cooperator and waiting for it to run out of work and shutdown.
//
struct Thread
{
    Thread(Cooperator* m);
    ~Thread();

    Cooperator* m_cooperator;
    std::thread m_thread;
};

}
