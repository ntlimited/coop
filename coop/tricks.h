#pragma once

#include <cstdint>

namespace coop
{

namespace detail
{

// This is a little piece of magic that skips the overhead of dynamic cast for cases where we are
// 100000% sure that the "real" class is Child for the pointer.
//
// Principally, this is relevant for EmbeddedList usecases.
//
template<typename Child, typename Base>
static constexpr Child* ascend_cast(Base* b)
{
    auto rawOffset = reinterpret_cast<ptrdiff_t>((Base*)((Child*)0x1000)) - (ptrdiff_t)0x1000;
    return reinterpret_cast<Child*>(reinterpret_cast<uintptr_t>(b) - rawOffset);
}

} // end namespace coop::detail
} // end namespace coop
