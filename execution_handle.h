#pragma once

struct ExecutionContext;

// An ExecutionHandle allows interaction with a context from outside of the context itself. It is
// only safe to touch from within the manager's physical thread - it is not even safe to try to
// access the context or its properties otherwise.
//
// APIs that are intended to be invoked on a context externally should always be wired
// through the handle; APIs that are for a context to run within itself should live on the
// context
//
struct ExecutionHandle
{
    ExecutionHandle(ExecutionHandle const&) = delete;
    ExecutionHandle(ExecutionHandle&&) = delete;
    ExecutionHandle()
    {
    }

    void Kill();

    operator bool() const
    {
        return !!m_executionContext;
    }

	ExecutionContext* m_executionContext;
};
