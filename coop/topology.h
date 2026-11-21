#pragma once

#include <cstddef>
#include <vector>
#include <sched.h>

namespace coop
{

struct CpuInfo
{
    int cpu_id;
    int numa_node;
    int core_cpu_id = -1; // lowest sibling ID, including siblings outside the allowed set
};

struct NumaNodeInfo
{
    int node_id;
    std::vector<int> cpus;
};

struct Topology
{
    std::vector<CpuInfo> cpus;          // sorted by logical CPU ID; contains only allowed CPUs
    std::vector<NumaNodeInfo> nodes;

    int NumaNodeForCpu(int cpu_id) const;
};

// Discover the system topology. Reads from sysfs + sched_getaffinity to determine which
// cores are available to this process and which NUMA nodes they belong to. Called lazily
// on first use and cached.
//
Topology const& GetTopology();

enum class CpuPolicy { CpuOrder, CoresFirst, SiblingsFirst, NoSmt };

// Build an assignment cycle from the already filtered topology. Topology-aware policies
// fill NUMA nodes in node-ID order. Incomplete core information preserves CPU order.
// NoSmt cycles over one allowed CPU per physical core, even when oversubscribed.
//
std::vector<int> CpuPlacementOrder(Topology const& topology, CpuPolicy policy);

namespace detail
{
// Discover from an affinity mask and a Linux system-device directory.
//
bool ParseCpuList(char const* str, cpu_set_t& out);
Topology DiscoverTopology(cpu_set_t const& available, char const* sysfsRoot);
}

// Returns the next CPU in the COOP_CPU_POLICY assignment cycle. Thread-safe.
// Environment and topology are sampled once, on first use.
//
int NextRoundRobinCpu();

// Pin the calling thread to the given logical CPU. Returns 0 on success, -1 on failure.
//
int PinThread(int cpu_id);

// Returns true if thread pinning is disabled via COOP_NO_PIN=1.
//
bool PinningDisabled();

} // end namespace coop
