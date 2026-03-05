#pragma once

#include "counters.h"

namespace coop
{
namespace perf
{

#if COOP_PERF_MODE == 2

// Enable all performance probes. Patches JMP instructions at probe sites to NOPs so counter
// increments fall through. Safe to call from any cooperator thread — patching is a series of
// atomic 2-byte writes visible after the next icache sync (guaranteed on x86 by the next
// taken branch or serializing instruction).
//
// Cooperative scheduling gives us an advantage: between context switches, only the cooperator
// thread executes, so probe sites are not being executed concurrently during patching.
//
void Enable();

// Disable all performance probes. Restores original JMP instructions at probe sites.
//
void Disable();

// Toggle probes on/off.
//
void Toggle();

// Returns true if probes are currently enabled.
//
bool IsEnabled();

// Returns the number of registered probe sites found in the coop_perf_sites section.
//
size_t ProbeCount();

#else

// Stubs for non-dynamic modes.
//
inline void Enable() {}
inline void Disable() {}
inline void Toggle() {}
inline bool IsEnabled() { return COOP_PERF_MODE == 1; }
inline size_t ProbeCount() { return 0; }

#endif

} // end namespace coop::perf
} // end namespace coop
