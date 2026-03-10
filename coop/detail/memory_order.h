#pragma once

#include <atomic>

// Architecture-aware memory ordering constants.
//
// On x86-64 (TSO), plain loads/stores have acquire/release semantics in hardware,
// so std::memory_order_relaxed is sufficient for flag-style atomics where we only
// need visibility (not reordering of surrounding non-atomic accesses). On weakly-
// ordered architectures (aarch64), we need explicit acquire/release barriers.
//
// These constants are for cross-thread flag variables (shutdown, submission
// signals) where the only requirement is timely visibility of the store — no
// surrounding data needs to be published through the flag.  For data-publishing
// patterns (e.g. epoch watermark, skip list pointers), use explicit
// memory_order_release / memory_order_acquire directly.
//
namespace coop::detail
{

#if defined(__x86_64__) || defined(_M_X64)
    inline constexpr auto kStoreFlag = std::memory_order_relaxed;
    inline constexpr auto kLoadFlag  = std::memory_order_relaxed;
#else
    inline constexpr auto kStoreFlag = std::memory_order_release;
    inline constexpr auto kLoadFlag  = std::memory_order_acquire;
#endif

} // namespace coop::detail
