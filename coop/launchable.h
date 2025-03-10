#pragma once

namespace coop
{

struct Context;

// Launchable allows for some mild syntactic sugar by skipping the spawn lambda
//
struct Launchable
{
    virtual ~Launchable()
    {
    }

    virtual void Launch() = 0;
};

} // end namespace coop
