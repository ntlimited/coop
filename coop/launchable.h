#pragma once

namespace coop
{

struct Context;

// Launchable provides an alternative to `Spawn` via the `Launch` API on the cooperator, which
// constructs an instance around a new context and calls its Launch method.
//
struct Launchable
{
    Launchable(Context* ctx)
    : m_context(ctx)
    {
    }

    virtual ~Launchable()
    {
    }

    virtual void Launch() = 0;

  protected:
    Context* GetContext()
    {
        return m_context;
    }

  private:
    Context* m_context;
};

} // end namespace coop
