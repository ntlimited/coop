#pragma once

namespace coop
{

struct Context;
struct Cooperator;

// Handle is the mechanism for working with contexts executing in a cooperator, both in and outsidee
// of cooperating contexts. Handle lifetimes must be guaranteed for as long as the execution of the
// spawned context.
//
// In theory, this is the obvious mechanism to add "return things" to the spawn concept. However,
// that's only really needed for outside-of-cooperator work that will probably want fancier ways
// to do things in general than what's easy to do right now, and the intra-context case, there
// isn't really any sufficiently magic syntax beyond union hijinks.
//
struct Handle
{
    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;
    Handle()
    {
    }

    void Kill();

    operator bool() const
    {
        return !!m_context;
    }

  private:
    friend Context;
    friend Cooperator;
	Context* m_context;
};

} // end namespace coop
