#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <pthread.h>
#include <dirent.h>
#include <algorithm>

#include "topology.h"

namespace coop
{

namespace
{

// Parse a cpulist string like "0-3,8,12-15" into a cpu_set_t.
//
void ParseCpuList(char const* str, cpu_set_t& out)
{
    CPU_ZERO(&out);

    char buf[4096];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* saveptr = nullptr;
    char* token = strtok_r(buf, ",\n", &saveptr);
    while (token)
    {
        int lo, hi;
        if (sscanf(token, "%d-%d", &lo, &hi) == 2)
        {
            for (int c = lo; c <= hi; c++)
            {
                CPU_SET(c, &out);
            }
        }
        else if (sscanf(token, "%d", &lo) == 1)
        {
            CPU_SET(lo, &out);
        }
        token = strtok_r(nullptr, ",\n", &saveptr);
    }
}

// Apply COOP_NUMA_NODE and COOP_CPUS env var filters to the discovered topology.
// COOP_NUMA_NODE=N restricts to CPUs on that NUMA node.
// COOP_CPUS=0-3,8 restricts to an explicit CPU set.
// Both compose (intersection).
//
void ApplyEnvFilters(Topology& topo)
{
    bool filtered = false;
    cpu_set_t allowed;
    CPU_ZERO(&allowed);

    // COOP_NUMA_NODE: restrict to a single NUMA node's CPUs
    //
    char const* numaEnv = getenv("COOP_NUMA_NODE");
    if (numaEnv)
    {
        int nodeId = -1;
        if (sscanf(numaEnv, "%d", &nodeId) == 1)
        {
            for (auto const& node : topo.nodes)
            {
                if (node.node_id == nodeId)
                {
                    for (int cpu : node.cpus)
                    {
                        CPU_SET(cpu, &allowed);
                    }
                    break;
                }
            }
            filtered = true;
        }
    }

    // COOP_CPUS: explicit CPU allowlist
    //
    char const* cpuEnv = getenv("COOP_CPUS");
    if (cpuEnv)
    {
        cpu_set_t cpuSet;
        ParseCpuList(cpuEnv, cpuSet);

        if (filtered)
        {
            // Intersect with existing filter
            //
            CPU_AND(&allowed, &allowed, &cpuSet);
        }
        else
        {
            allowed = cpuSet;
            filtered = true;
        }
    }

    if (!filtered)
    {
        return;
    }

    // Filter the cpu list down to only those in the allowed set
    //
    std::vector<CpuInfo> filteredCpus;
    for (auto const& info : topo.cpus)
    {
        if (CPU_ISSET(info.cpu_id, &allowed))
        {
            filteredCpus.push_back(info);
        }
    }
    topo.cpus = std::move(filteredCpus);

    // Filter each node's cpu list to match
    //
    for (auto& node : topo.nodes)
    {
        std::vector<int> filteredNodeCpus;
        for (int cpu : node.cpus)
        {
            if (CPU_ISSET(cpu, &allowed))
            {
                filteredNodeCpus.push_back(cpu);
            }
        }
        node.cpus = std::move(filteredNodeCpus);
    }

    // Remove empty nodes
    //
    topo.nodes.erase(
        std::remove_if(topo.nodes.begin(), topo.nodes.end(),
                        [](auto const& n) { return n.cpus.empty(); }),
        topo.nodes.end());
}

Topology DiscoverTopology()
{
    Topology topo;

    // Get the set of CPUs this process is allowed to use
    //
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available) != 0)
    {
        return topo;
    }

    // First pass: record all available CPUs. NUMA node assignment is backfilled from the
    // node-side scan below.
    //
    int maxCpu = CPU_SETSIZE;
    for (int cpu = 0; cpu < maxCpu; cpu++)
    {
        if (!CPU_ISSET(cpu, &available))
        {
            continue;
        }

        CpuInfo info;
        info.cpu_id = cpu;
        info.numa_node = 0; // filled in below
        topo.cpus.push_back(info);
    }

    // Now discover NUMA nodes and assign cpus to them
    //
    DIR* nodeDir = opendir("/sys/devices/system/node");
    if (nodeDir)
    {
        struct dirent* entry;
        while ((entry = readdir(nodeDir)) != nullptr)
        {
            if (strncmp(entry->d_name, "node", 4) != 0)
            {
                continue;
            }

            int nodeId = -1;
            if (sscanf(entry->d_name, "node%d", &nodeId) != 1 || nodeId < 0)
            {
                continue;
            }

            // Read the cpulist for this node. Format is like "0-23,48-71"
            //
            char path[128];
            snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", nodeId);

            FILE* f = fopen(path, "r");
            if (!f) continue;

            char buf[4096];
            if (!fgets(buf, sizeof(buf), f))
            {
                fclose(f);
                continue;
            }
            fclose(f);

            // Parse the cpulist format: comma-separated ranges like "0-23,48-71"
            //
            NumaNodeInfo nodeInfo;
            nodeInfo.node_id = nodeId;

            char* saveptr = nullptr;
            char* token = strtok_r(buf, ",\n", &saveptr);
            while (token)
            {
                int lo, hi;
                if (sscanf(token, "%d-%d", &lo, &hi) == 2)
                {
                    for (int c = lo; c <= hi; c++)
                    {
                        if (CPU_ISSET(c, &available))
                        {
                            nodeInfo.cpus.push_back(c);
                        }
                    }
                }
                else if (sscanf(token, "%d", &lo) == 1)
                {
                    if (CPU_ISSET(lo, &available))
                    {
                        nodeInfo.cpus.push_back(lo);
                    }
                }
                token = strtok_r(nullptr, ",\n", &saveptr);
            }

            if (!nodeInfo.cpus.empty())
            {
                topo.nodes.push_back(std::move(nodeInfo));
            }
        }
        closedir(nodeDir);
    }

    // Sort nodes by id for predictable ordering
    //
    std::sort(topo.nodes.begin(), topo.nodes.end(),
              [](auto const& a, auto const& b) { return a.node_id < b.node_id; });

    // Now backfill the numa_node field on each CpuInfo
    //
    for (auto const& node : topo.nodes)
    {
        for (int cpu : node.cpus)
        {
            for (auto& info : topo.cpus)
            {
                if (info.cpu_id == cpu)
                {
                    info.numa_node = node.node_id;
                    break;
                }
            }
        }
    }

    // If we found no NUMA nodes (container, weird sysfs), synthesize one with all cpus
    //
    if (topo.nodes.empty() && !topo.cpus.empty())
    {
        NumaNodeInfo fallback;
        fallback.node_id = 0;
        for (auto const& info : topo.cpus)
        {
            fallback.cpus.push_back(info.cpu_id);
        }
        topo.nodes.push_back(std::move(fallback));
    }

    ApplyEnvFilters(topo);

    return topo;
}

std::atomic<int> s_roundRobinCounter{0};

} // end anonymous namespace

int Topology::NumaNodeForCpu(int cpu_id) const
{
    for (auto const& info : cpus)
    {
        if (info.cpu_id == cpu_id)
        {
            return info.numa_node;
        }
    }
    return -1;
}

Topology const& GetTopology()
{
    static Topology topo = DiscoverTopology();
    return topo;
}

int NextRoundRobinCpu()
{
    auto const& topo = GetTopology();
    if (topo.cpus.empty())
    {
        return -1;
    }

    int idx = s_roundRobinCounter.fetch_add(1, std::memory_order_relaxed);
    return topo.cpus[idx % topo.cpus.size()].cpu_id;
}

int PinThread(int cpu_id)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

bool PinningDisabled()
{
    static bool disabled = []()
    {
        char const* env = getenv("COOP_NO_PIN");
        return env && strcmp(env, "1") == 0;
    }();
    return disabled;
}

} // end namespace coop
