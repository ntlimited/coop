#pragma once

namespace coop
{

struct Context;

// A Handle allows interaction with a context from outside of the context itself. It is
// only safe to touch from within the cooperator's physical thread - it is not even safe to try to
// access the context or its properties otherwise.
//
// APIs that are intended to be invoked on a context externally should always be wired
// through the handle; APIs that are for a context to run within itself should live on the
// context
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
	Context* m_context;
};

} // end namespace coop
