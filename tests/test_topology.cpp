#include <gtest/gtest.h>

#include <cstdlib>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include "coop/topology.h"
#include "coop/cooperator.h"
#include "coop/self.h"

#include "test_helpers.h"

// GetTopology() and PinningDisabled() use lazily-initialized statics, so env var tests
// must run in fresh processes. We re-exec the test binary with the env var set and a
// gtest filter targeting a "Sub" test that does the actual assertion.
//

namespace
{

// Re-exec this test binary with env vars set and a specific gtest filter.
// Returns the child's exit code (0 = all tests passed).
//
int RunWithEnv(char const* filter,
               std::initializer_list<std::pair<char const*, char const*>> envVars)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        for (auto const& [key, val] : envVars)
        {
            setenv(key, val, 1);
        }

        // Get the path to this binary from /proc/self/exe
        //
        char self[4096];
        ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len <= 0) _exit(99);
        self[len] = '\0';

        char filterArg[256];
        snprintf(filterArg, sizeof(filterArg), "--gtest_filter=%s", filter);

        execl(self, self, filterArg, nullptr);
        _exit(98); // exec failed
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1;
}

} // end anonymous namespace

TEST(TopologyTest, DiscoversCpus)
{
    auto const& topo = coop::GetTopology();

    // We must have at least one CPU available
    //
    EXPECT_GT(topo.cpus.size(), 0u);
}

TEST(TopologyTest, HasAtLeastOneNode)
{
    auto const& topo = coop::GetTopology();
    EXPECT_GT(topo.nodes.size(), 0u);
}

TEST(TopologyTest, AllCpusBelongToANode)
{
    auto const& topo = coop::GetTopology();
    for (auto const& info : topo.cpus)
    {
        bool found = false;
        for (auto const& node : topo.nodes)
        {
            for (int cpu : node.cpus)
            {
                if (cpu == info.cpu_id)
                {
                    found = true;
                    EXPECT_EQ(info.numa_node, node.node_id);
                }
            }
        }
        EXPECT_TRUE(found) << "cpu " << info.cpu_id << " not found in any node";
    }
}

TEST(TopologyTest, NumaNodeForCpu)
{
    auto const& topo = coop::GetTopology();
    if (topo.cpus.empty()) GTEST_SKIP();

    int cpu = topo.cpus[0].cpu_id;
    int node = topo.NumaNodeForCpu(cpu);
    EXPECT_EQ(node, topo.cpus[0].numa_node);

    // Non-existent CPU returns -1
    //
    EXPECT_EQ(topo.NumaNodeForCpu(99999), -1);
}

TEST(TopologyTest, NextRoundRobinCycles)
{
    auto const& topo = coop::GetTopology();
    if (topo.cpus.size() < 2) GTEST_SKIP();

    // Grab enough CPUs to see a full cycle. The round-robin counter is global and
    // may have been incremented by earlier tests/cooperators, so just verify we get
    // valid CPU IDs from the topology.
    //
    size_t n = topo.cpus.size();
    for (size_t i = 0; i < n * 2; i++)
    {
        int cpu = coop::NextRoundRobinCpu();
        EXPECT_GE(cpu, 0);

        bool valid = false;
        for (auto const& info : topo.cpus)
        {
            if (info.cpu_id == cpu)
            {
                valid = true;
                break;
            }
        }
        EXPECT_TRUE(valid) << "round-robin returned cpu " << cpu << " not in topology";
    }
}

TEST(TopologyTest, PinThread)
{
    auto const& topo = coop::GetTopology();
    if (topo.cpus.empty()) GTEST_SKIP();

    int cpu = topo.cpus[0].cpu_id;
    EXPECT_EQ(coop::PinThread(cpu), 0);

    // Verify we're actually pinned
    //
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof(set), &set);
    EXPECT_TRUE(CPU_ISSET(cpu, &set));
    EXPECT_EQ(CPU_COUNT(&set), 1);

    // Restore: pin to all available CPUs so we don't affect other tests
    //
    CPU_ZERO(&set);
    for (auto const& info : topo.cpus)
    {
        CPU_SET(info.cpu_id, &set);
    }
    sched_setaffinity(0, sizeof(set), &set);
}

TEST(TopologyTest, CooperatorGetsCpuAndNuma)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto* co = ctx->GetCooperator();

        // With default config, the cooperator should have been pinned
        //
        if (!coop::PinningDisabled())
        {
            EXPECT_GE(co->CpuId(), 0);
            EXPECT_GE(co->NumaNode(), 0);
        }
    });
}

// --- Subprocess env var tests ---
// "Sub" tests are the actual assertions, run in a child process with the env var set.
// The wrapper tests invoke them via RunWithEnv and check the exit code.
//

TEST(TopologySubTest, NoPinDisabled)
{
    // Only runs meaningfully when COOP_NO_PIN is set (via RunWithEnv)
    //
    if (!getenv("COOP_NO_PIN")) GTEST_SKIP();
    EXPECT_TRUE(coop::PinningDisabled());
}

TEST(TopologyTest, EnvCoopNoPin)
{
    EXPECT_EQ(RunWithEnv("TopologySubTest.NoPinDisabled", {{"COOP_NO_PIN", "1"}}), 0);
}

TEST(TopologySubTest, CpuFilterSingle)
{
    char const* env = getenv("COOP_CPUS");
    if (!env) GTEST_SKIP();

    int targetCpu = -1;
    sscanf(env, "%d", &targetCpu);

    auto const& topo = coop::GetTopology();
    ASSERT_EQ(topo.cpus.size(), 1u);
    EXPECT_EQ(topo.cpus[0].cpu_id, targetCpu);
}

TEST(TopologyTest, EnvCoopCpusFilters)
{
    auto const& topo = coop::GetTopology();
    if (topo.cpus.size() < 2) GTEST_SKIP();

    char cpuStr[32];
    snprintf(cpuStr, sizeof(cpuStr), "%d", topo.cpus[0].cpu_id);

    EXPECT_EQ(RunWithEnv("TopologySubTest.CpuFilterSingle", {{"COOP_CPUS", cpuStr}}), 0);
}

TEST(TopologySubTest, CpuFilterRange)
{
    char const* env = getenv("COOP_CPUS");
    if (!env) GTEST_SKIP();

    int lo = -1, hi = -1;
    sscanf(env, "%d-%d", &lo, &hi);

    auto const& topo = coop::GetTopology();
    ASSERT_GT(topo.cpus.size(), 0u);
    for (auto const& info : topo.cpus)
    {
        EXPECT_GE(info.cpu_id, lo);
        EXPECT_LE(info.cpu_id, hi);
    }
}

TEST(TopologyTest, EnvCoopCpusRange)
{
    auto const& topo = coop::GetTopology();
    if (topo.cpus.size() < 3) GTEST_SKIP();

    char rangeStr[64];
    snprintf(rangeStr, sizeof(rangeStr), "%d-%d",
             topo.cpus[0].cpu_id, topo.cpus[1].cpu_id);

    EXPECT_EQ(RunWithEnv("TopologySubTest.CpuFilterRange", {{"COOP_CPUS", rangeStr}}), 0);
}

TEST(TopologySubTest, NumaNodeFilter)
{
    char const* env = getenv("COOP_NUMA_NODE");
    if (!env) GTEST_SKIP();

    int nodeId = -1;
    sscanf(env, "%d", &nodeId);

    auto const& topo = coop::GetTopology();
    ASSERT_GT(topo.cpus.size(), 0u);

    for (auto const& info : topo.cpus)
    {
        EXPECT_EQ(info.numa_node, nodeId);
    }
    ASSERT_EQ(topo.nodes.size(), 1u);
    EXPECT_EQ(topo.nodes[0].node_id, nodeId);
}

TEST(TopologyTest, EnvCoopNumaNode)
{
    auto const& topo = coop::GetTopology();
    if (topo.nodes.empty()) GTEST_SKIP();

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", topo.nodes[0].node_id);

    EXPECT_EQ(RunWithEnv("TopologySubTest.NumaNodeFilter", {{"COOP_NUMA_NODE", buf}}), 0);
}

TEST(TopologySubTest, NumaNodeInvalid)
{
    char const* env = getenv("COOP_NUMA_NODE");
    if (!env) GTEST_SKIP();

    auto const& topo = coop::GetTopology();
    EXPECT_TRUE(topo.cpus.empty());
}

TEST(TopologyTest, EnvCoopNumaNodeInvalidIsEmpty)
{
    EXPECT_EQ(RunWithEnv("TopologySubTest.NumaNodeInvalid", {{"COOP_NUMA_NODE", "9999"}}), 0);
}
