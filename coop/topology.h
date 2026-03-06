#pragma once

#include <cstddef>
#include <vector>

namespace coop
{

struct CpuInfo
{
    int cpu_id;
    int numa_node;
};

struct NumaNodeInfo
{
    int node_id;
    std::vector<int> cpus;
};

struct Topology
{
    std::vector<CpuInfo> cpus;          // indexed by logical cpu id (sparse gaps possible)
    std::vector<NumaNodeInfo> nodes;

    int NumaNodeForCpu(int cpu_id) const;
};

// Discover the system topology. Reads from sysfs + sched_getaffinity to determine which
// cores are available to this process and which NUMA nodes they belong to. Called lazily
// on first use and cached.
//
Topology const& GetTopology();

// Returns the next CPU ID for round-robin assignment across available cores. Thread-safe.
//
int NextRoundRobinCpu();

// Pin the calling thread to the given logical CPU. Returns 0 on success, -1 on failure.
//
int PinThread(int cpu_id);

// Returns true if thread pinning is disabled via COOP_NO_PIN=1.
//
bool PinningDisabled();

} // end namespace coop
