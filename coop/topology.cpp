#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <pthread.h>
#include <dirent.h>
#include <algorithm>
#include <cerrno>
#include <map>
#include <string>

#include "topology.h"

namespace coop
{

namespace detail
{

bool ParseCpuList(char const* str, cpu_set_t& out)
{
    CPU_ZERO(&out);
    cpu_set_t parsed;
    CPU_ZERO(&parsed);
    if (!str || !*str) return false;
    char const* p = str;
    while (*p)
    {
        if (*p < '0' || *p > '9') return false;
        char* end;
        errno = 0;
        long lo = strtol(p, &end, 10);
        if (errno || lo < 0 || lo >= CPU_SETSIZE) return false;
        p = end;
        long hi = lo;
        if (*p == '-')
        {
            ++p;
            if (*p < '0' || *p > '9') return false;
            errno = 0;
            hi = strtol(p, &end, 10);
            if (errno || hi < lo || hi >= CPU_SETSIZE) return false;
            p = end;
        }
        for (int cpu = lo; cpu <= hi; ++cpu) CPU_SET(cpu, &parsed);
        if (*p == ',')
        {
            ++p;
            if (!*p) return false;
        }
        else
        {
            if (*p == '\n') ++p;
            if (*p) return false;
            break;
        }
    }
    out = parsed;
    return true;
}

} // end namespace detail

namespace
{

// Preserve the environment CPU-list grammar: comma/newline tokens, sscanf numeric
// prefixes, ignored unrecognized tokens and the 4095-byte input limit. Sysfs input
// uses the strict parser instead, since partial sibling identities are unsafe.
//
void ParseEnvCpuList(char const* str, cpu_set_t& out)
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

bool ReadCpuList(std::string const& path, cpu_set_t& out)
{
    FILE* file = fopen(path.c_str(), "r");
    if (!file) return false;
    char buf[4096];
    bool ok = fgets(buf, sizeof(buf), file) != nullptr;
    // Reject trailing contents and truncated lists rather than trusting a partial identity.
    //
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return ok && detail::ParseCpuList(buf, out);
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
        else
        {
            fprintf(stderr, "Invalid COOP_NUMA_NODE; ignoring NUMA filter\n");
        }
    }

    // COOP_CPUS: explicit CPU allowlist
    //
    char const* cpuEnv = getenv("COOP_CPUS");
    if (cpuEnv)
    {
        cpu_set_t cpuSet;
        ParseEnvCpuList(cpuEnv, cpuSet);

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
    if (topo.cpus.empty())
    {
        fprintf(stderr, "COOP_CPUS/COOP_NUMA_NODE select no CPUs; automatic pinning is disabled\n");
    }

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

} // end anonymous namespace

Topology detail::DiscoverTopology(cpu_set_t const& affinity, char const* sysfsRoot)
{
    Topology topo;
    std::string root(sysfsRoot);
    cpu_set_t available = affinity;
    cpu_set_t online;
    if (ReadCpuList(root + "/cpu/online", online))
        CPU_AND(&available, &available, &online);
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
    DIR* nodeDir = opendir((root + "/node").c_str());
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
            cpu_set_t nodeCpus;
            if (!ReadCpuList(root + "/node/node" + std::to_string(nodeId) + "/cpulist",
                             nodeCpus)) continue;
            NumaNodeInfo nodeInfo;
            nodeInfo.node_id = nodeId;
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            {
                if (CPU_ISSET(cpu, &available) && CPU_ISSET(cpu, &nodeCpus))
                    nodeInfo.cpus.push_back(cpu);
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

    // Sibling IDs are stable core identities even when the lowest sibling is offline
    // or outside the process affinity. Only allowed CPUs enter the placement cycle.
    // Require complete, reciprocal information; partial container sysfs must not guess.
    //
    std::vector<cpu_set_t> siblings(topo.cpus.size());
    bool complete = true;
    for (size_t i = 0; i < topo.cpus.size(); ++i)
    {
        auto& info = topo.cpus[i];
        if (!ReadCpuList(root + "/cpu/cpu" + std::to_string(info.cpu_id) +
                         "/topology/thread_siblings_list", siblings[i]) ||
            !CPU_ISSET(info.cpu_id, &siblings[i]))
        {
            complete = false;
            break;
        }
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        {
            if (CPU_ISSET(cpu, &siblings[i]))
            {
                info.core_cpu_id = cpu;
                break;
            }
        }
    }
    if (complete)
    {
        for (size_t i = 0; i < topo.cpus.size(); ++i)
        {
            for (size_t j = 0; j < i; ++j)
            {
                cpu_set_t overlap;
                CPU_AND(&overlap, &siblings[i], &siblings[j]);
                if (CPU_COUNT(&overlap) != 0)
                {
                    if (!CPU_EQUAL(&siblings[i], &siblings[j]) ||
                        topo.cpus[i].numa_node != topo.cpus[j].numa_node) complete = false;
                }
            }
        }
    }
    if (!complete)
        for (auto& cpu : topo.cpus) cpu.core_cpu_id = -1;

    return topo;
}

namespace
{

std::atomic<size_t> s_roundRobinCounter{0};

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
    static Topology topo = []()
    {
        cpu_set_t available;
        CPU_ZERO(&available);
        if (sched_getaffinity(0, sizeof(available), &available) != 0) return Topology{};
        return detail::DiscoverTopology(available, "/sys/devices/system");
    }();
    return topo;
}

std::vector<int> CpuPlacementOrder(Topology const& topology, CpuPolicy policy)
{
    std::vector<int> order;
    for (auto const& cpu : topology.cpus) order.push_back(cpu.cpu_id);
    std::sort(order.begin(), order.end());
    if (policy == CpuPolicy::CpuOrder ||
        std::any_of(topology.cpus.begin(), topology.cpus.end(),
                    [](auto const& cpu) { return cpu.core_cpu_id < 0; })) return order;

    // Node, core and sibling ordering is deterministic, independent of sysfs scan order.
    //
    std::map<int, std::map<int, std::vector<int>>> nodes;
    for (auto const& cpu : topology.cpus)
        nodes[cpu.numa_node][cpu.core_cpu_id].push_back(cpu.cpu_id);
    order.clear();
    for (auto& [node, cores] : nodes)
    {
        size_t width = 0;
        for (auto& [core, cpus] : cores)
        {
            std::sort(cpus.begin(), cpus.end());
            width = std::max(width, cpus.size());
            if (policy == CpuPolicy::SiblingsFirst)
                order.insert(order.end(), cpus.begin(), cpus.end());
        }
        if (policy == CpuPolicy::SiblingsFirst) continue;
        if (policy == CpuPolicy::NoSmt) width = std::min(width, size_t{1});
        for (size_t sibling = 0; sibling < width; ++sibling)
            for (auto const& [core, cpus] : cores)
                if (sibling < cpus.size()) order.push_back(cpus[sibling]);
    }
    return order;
}

int NextRoundRobinCpu()
{
    static auto const order = []()
    {
        CpuPolicy policy = CpuPolicy::CpuOrder;
        char const* env = getenv("COOP_CPU_POLICY");
        if (env && strcmp(env, "cpu-order") != 0)
        {
            if (strcmp(env, "cores-first") == 0) policy = CpuPolicy::CoresFirst;
            else if (strcmp(env, "siblings-first") == 0) policy = CpuPolicy::SiblingsFirst;
            else if (strcmp(env, "no-smt") == 0) policy = CpuPolicy::NoSmt;
            else
            {
                fprintf(stderr, "Invalid COOP_CPU_POLICY '%s'; expected cpu-order, "
                                "cores-first, siblings-first, or no-smt\n", env);
                abort();
            }
        }
        return CpuPlacementOrder(GetTopology(), policy);
    }();
    if (order.empty()) return -1;
    size_t idx = s_roundRobinCounter.fetch_add(1, std::memory_order_relaxed);
    return order[idx % order.size()];
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
