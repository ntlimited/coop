#pragma once

#include "counters.h"

namespace coop
{
namespace perf
{

#if COOP_PERF_MODE == 2

// Enable performance probes for the given families. Patches JMP instructions at matching
// probe sites to NOPs so counter increments fall through. Only patches sites whose enabled
// state actually changes, so toggling one family doesn't re-patch the others.
//
void Enable(Family families = Family::All);

// Disable performance probes for the given families. Restores original JMP instructions.
//
void Disable(Family families = Family::All);

// Set exactly this family mask — enable families in the mask, disable families not in it.
//
void SetFamilies(Family families);

// Toggle all probes on/off.
//
void Toggle();

// Returns the currently enabled family bitmask.
//
Family EnabledFamilies();

// Returns true if any probes are currently enabled.
//
bool IsEnabled();

// Returns the number of registered probe sites found in the coop_perf_sites section.
//
size_t ProbeCount();

#else

// Stubs for non-dynamic modes.
//
inline void Enable(Family = Family::All) {}
inline void Disable(Family = Family::All) {}
inline void SetFamilies(Family) {}
inline void Toggle() {}
inline Family EnabledFamilies() { return COOP_PERF_MODE == 1 ? Family::All : Family{}; }
inline bool IsEnabled() { return COOP_PERF_MODE == 1; }
inline size_t ProbeCount() { return 0; }

#endif

} // end namespace coop::perf
} // end namespace coop
