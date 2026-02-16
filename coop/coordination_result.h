#pragma once

#include "coop/coordinator.h"

namespace coop
{

// CoordinationResult is the return type of CoordinateWith. It carries the coordinator that was
// acquired, along with sentinel detection for kill/timeout/error conditions.
//
// Prefer pointer comparison (operator==) over inspecting the index field directly.
//
struct CoordinationResult
{
    size_t          index;
    Coordinator*    coordinator;

    bool Killed() const { return index == static_cast<size_t>(-1); }

    bool TimedOut() const { return index == static_cast<size_t>(-2); }

    bool Error() const { return index == static_cast<size_t>(-3); }

    operator Coordinator*()
    {
        return coordinator;
    }

    Coordinator* operator->()
    {
        return coordinator;
    }

    bool operator==(Coordinator* rhs) const
    {
        return coordinator == rhs;
    }

    bool operator==(const Coordinator& rhs) const
    {
        return coordinator == &rhs;
    }
};

} // end namespace coop
