#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{

struct Context;

namespace perf
{

// CPU sampling profiler for cooperative contexts.
//
// Uses SIGPROF + ITIMER_PROF to periodically capture the instruction pointer and attribute it
// to the currently-running context. Samples are stored in a lock-free ring buffer (the signal
// handler is async-signal-safe — just an atomic index bump and struct write). Per-context sample
// counts are also maintained directly in Context::m_statistics.
//
// This is independent of the perf counter system (COOP_PERF_MODE). It can be enabled/disabled
// at runtime via API, env var (COOP_SAMPLE=<hz>), or programmatically.
//
// Thread safety: the signal fires on the cooperator thread that consumed CPU time. The handler
// reads the thread-local cooperator's m_scheduled pointer to attribute samples. With multiple
// cooperator threads, each gets its own signals and reads its own thread-local state.
//

struct Sample
{
    uintptr_t pc;           // instruction pointer at sample time
    Context*  context;      // currently-running context (null = cooperator code)
    uint64_t  timestamp;    // rdtsc at sample time
};

// Start sampling at the given frequency (Hz). Common values: 99, 997 (primes avoid aliasing
// with periodic workloads). Installs SIGPROF handler and arms ITIMER_PROF. Safe to call
// multiple times (updates frequency). Returns false on error.
//
bool StartSampling(int hz = 99);

// Stop sampling. Disarms the timer and restores the default SIGPROF handler.
//
void StopSampling();

// Returns true if sampling is currently active.
//
bool IsSampling();

// Returns the configured sampling frequency in Hz (0 if not sampling).
//
int SamplingHz();

// Read up to `maxSamples` from the ring buffer into `out`. Returns the number of samples
// written. Samples are returned oldest-first. This is a snapshot — new samples may arrive
// while reading, but the returned data is consistent (each sample is a complete struct).
//
size_t ReadSamples(Sample* out, size_t maxSamples);

// Reset the ring buffer and total count. Call before StartSampling to get a clean window.
//
void ResetSamples();

// Total number of samples captured since sampling was last started.
//
size_t TotalSamples();

// Ring buffer capacity.
//
size_t SampleCapacity();

} // end namespace coop::perf
} // end namespace coop
