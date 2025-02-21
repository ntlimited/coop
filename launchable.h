#pragma once

struct ExecutionContext;

// Launchable allows for some mild syntactic sugar by skipping the spawn lambda
//
struct Launchable
{
    virtual ~Launchable()
    {
    }

    virtual void Launch(ExecutionContext*) = 0;
};
