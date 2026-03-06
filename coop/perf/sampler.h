#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{

struct Context;
struct Cooperator;

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
// cooperator threads, each gets its own signals and reads its own thread-local state. Each sample
// includes both the Context* and Cooperator* that were active at sample time, enabling
// per-cooperator flame graph grouping in multi-cooperator processes.
//

struct Sample
{
    uintptr_t    pc;           // instruction pointer at sample time
    Context*     context;      // currently-running context (null = cooperator code)
    Cooperator*  cooperator;   // cooperator thread that was sampled (null = no cooperator)
    uint64_t     timestamp;    // rdtsc at sample time
};

static constexpr int MAX_STACK_DEPTH = 32;

struct StackSample
{
    uintptr_t    frames[MAX_STACK_DEPTH];
    uint8_t      depth;        // number of valid frames
    Context*     context;
    Cooperator*  cooperator;
    uint64_t     timestamp;
};

// Start sampling at the given frequency (Hz). Common values: 99, 997 (primes avoid aliasing
// with periodic workloads). Installs SIGPROF handler and arms ITIMER_PROF. Safe to call
// multiple times (updates frequency). When stacks=true, also captures call stacks via frame
// pointer walking (every Nth signal, configurable via SetStackSubsample). Returns false on
// error.
//
bool StartSampling(int hz = 99, bool stacks = false);

// Stop sampling. Disarms the timer and restores the default SIGPROF handler.
//
void StopSampling();

// Returns true if sampling is currently active.
//
bool IsSampling();

// Returns true if the current (or most recent) sampling session used stack mode.
//
bool IsStackMode();

// Returns the configured sampling frequency in Hz (0 if not sampling).
//
int SamplingHz();

// Read up to `maxSamples` from the ring buffer into `out`. Returns the number of samples
// written. Samples are returned oldest-first. This is a snapshot — new samples may arrive
// while reading, but the returned data is consistent (each sample is a complete struct).
//
size_t ReadSamples(Sample* out, size_t maxSamples);

// Read up to `maxSamples` stack samples. Only valid when IsStackMode() is true.
//
size_t ReadStackSamples(StackSample* out, size_t maxSamples);

// Reset the ring buffer and total count. Call before StartSampling to get a clean window.
//
void ResetSamples();

// Configure stack subsample ratio — frame pointer walk runs every Nth signal (default 10).
// Other signals still record RIP to the PC ring. Lower values = more stacks but more overhead.
//
void SetStackSubsample(int every);
int StackSubsample();

// Total number of samples captured since sampling was last started.
//
size_t TotalSamples();

// Ring buffer capacity (for the active mode's ring).
//
size_t SampleCapacity();

} // end namespace coop::perf
} // end namespace coop
