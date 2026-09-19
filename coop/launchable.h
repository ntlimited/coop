#pragma once

namespace coop
{

struct Context;

// Result of Cooperator::Launch. `spawned` is whether a context was created and entered.
// `object` is the Launchable only if it yielded (still alive). A Launchable that returns
// from Launch() before yielding is spawned==true and object==nullptr — the instance is gone.
// operator bool is spawned, so `if (!Launch<T>(...))` means spawn failed, not "already done".
//
template<typename T>
struct LaunchResult
{
    T* object{nullptr};
    bool spawned{false};

    explicit operator bool() const { return spawned; }
};

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
