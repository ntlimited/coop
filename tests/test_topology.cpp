#include <gtest/gtest.h>

#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
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
    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        for (auto const& [key, val] : envVars)
        {
            if (val)
                setenv(key, val, 1);
            else
                unsetenv(key);
        }

        // Get the path to this binary from /proc/self/exe
        //
        char self[4096];
        ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len <= 0)
            _exit(99);
        self[len] = '\0';

        char filterArg[256];
        snprintf(filterArg, sizeof(filterArg), "--gtest_filter=%s", filter);

        execl(self, self, filterArg, nullptr);
        _exit(98); // exec failed
    }

    int status = 0;
    pid_t waited;
    do
    {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid)
        return -1;
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1;
}

class ScopedEnv
{
public:
    ScopedEnv(char const* key, char const* value) : m_key(key)
    {
        char const* old = getenv(key);
        m_present = old != nullptr;
        if (old)
            m_value = old;
        if (value)
            setenv(key, value, 1);
        else
            unsetenv(key);
    }

    ~ScopedEnv()
    {
        if (m_present)
            setenv(m_key, m_value.c_str(), 1);
        else
            unsetenv(m_key);
    }

private:
    char const* m_key;
    std::string m_value;
    bool m_present;
};

cpu_set_t CpuSet(std::initializer_list<int> cpus)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : cpus)
        CPU_SET(cpu, &set);
    return set;
}

void ExpectPlacementOrders(coop::Topology const& topology, std::vector<int> cpuOrder,
                           std::vector<int> coresFirst, std::vector<int> siblingsFirst,
                           std::vector<int> noSmt)
{
    EXPECT_EQ(coop::CpuPlacementOrder(topology, coop::CpuPolicy::CpuOrder), cpuOrder);
    EXPECT_EQ(coop::CpuPlacementOrder(topology, coop::CpuPolicy::CoresFirst), coresFirst);
    EXPECT_EQ(coop::CpuPlacementOrder(topology, coop::CpuPolicy::SiblingsFirst), siblingsFirst);
    EXPECT_EQ(coop::CpuPlacementOrder(topology, coop::CpuPolicy::NoSmt), noSmt);
}

coop::CpuPolicy EnvironmentPolicy()
{
    char const* value = getenv("COOP_CPU_POLICY");
    std::string name = value ? value : "cpu-order";
    if (name == "cores-first")
        return coop::CpuPolicy::CoresFirst;
    if (name == "siblings-first")
        return coop::CpuPolicy::SiblingsFirst;
    if (name == "no-smt")
        return coop::CpuPolicy::NoSmt;
    return coop::CpuPolicy::CpuOrder;
}

class TopologyDiscoveryTest : public testing::Test
{
protected:
    void SetUp() override
    {
        char path[] = "/tmp/coop-topology-XXXXXX";
        char* root = mkdtemp(path);
        ASSERT_NE(root, nullptr);
        m_root = root;
    }

    ~TopologyDiscoveryTest() override
    {
        // This fixture owns only the unique directory returned by mkdtemp.
        //
        if (!m_root.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
        }
    }

    void Write(std::string const& path, std::string const& contents)
    {
        auto target = m_root / path;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target);
        ASSERT_TRUE(file.is_open());
        file << contents;
        ASSERT_TRUE(file.good());
    }

    void Siblings(int cpu, std::string const& list)
    {
        Write("cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list", list);
    }

    coop::Topology Discover(std::initializer_list<int> cpus)
    {
        return coop::detail::DiscoverTopology(CpuSet(cpus), m_root.c_str());
    }

    void ExpectUnknown(coop::Topology const& topology)
    {
        ASSERT_FALSE(topology.cpus.empty());
        for (auto const& cpu : topology.cpus)
            EXPECT_EQ(cpu.core_cpu_id, -1);
    }

    std::filesystem::path m_root;
    ScopedEnv m_cpus{"COOP_CPUS", nullptr};
    ScopedEnv m_node{"COOP_NUMA_NODE", nullptr};
};

} // end anonymous namespace

TEST(TopologyTest, ParsesCpuLists)
{
    for (char const* list : {"0-3,8,10-11", "0-3,8,10-11\n", "0,1-3,8,10,11,11"})
    {
        SCOPED_TRACE(list);
        cpu_set_t actual;
        auto expected = CpuSet({0, 1, 2, 3, 8, 10, 11});
        ASSERT_TRUE(coop::detail::ParseCpuList(list, actual));
        EXPECT_TRUE(CPU_EQUAL(&actual, &expected));
    }
    cpu_set_t boundary;
    ASSERT_TRUE(coop::detail::ParseCpuList(std::to_string(CPU_SETSIZE - 1).c_str(), boundary));
    EXPECT_EQ(CPU_COUNT(&boundary), 1);
    EXPECT_TRUE(CPU_ISSET(CPU_SETSIZE - 1, &boundary));
}

TEST(TopologyTest, RejectsInvalidCpuListsWithoutPartialResults)
{
    std::vector<char const*> invalid = {
        nullptr, "",      "\n", "-1",   "+1",    " 1",
        "1 ",    "1,",    ",1", "1,,2", "3-1",   "1-",
        "1--2",  "1-2-3", "1x", "1\n2", "1\n\n", "1,99999999999999999999999999"};
    auto outside = "0," + std::to_string(CPU_SETSIZE);
    auto rangeOutside = "0-" + std::to_string(CPU_SETSIZE);
    invalid.push_back(outside.c_str());
    invalid.push_back(rangeOutside.c_str());
    for (char const* list : invalid)
    {
        SCOPED_TRACE(list ? list : "null");
        auto actual = CpuSet({0, 1});
        EXPECT_FALSE(coop::detail::ParseCpuList(list, actual));
        EXPECT_EQ(CPU_COUNT(&actual), 0);
    }
}

TEST(TopologyTest, PlacementAdjacentSiblings)
{
    coop::Topology topology{{{3, 0, 2}, {1, 0, 0}, {0, 0, 0}, {2, 0, 2}}, {}};
    ExpectPlacementOrders(topology, {0, 1, 2, 3}, {0, 2, 1, 3}, {0, 1, 2, 3}, {0, 2});
}

TEST(TopologyTest, PlacementUpperHalfSiblings)
{
    coop::Topology topology{
        {{0, 0, 0}, {1, 0, 1}, {2, 0, 2}, {3, 0, 3}, {4, 0, 0}, {5, 0, 1}, {6, 0, 2}, {7, 0, 3}},
        {}};
    ExpectPlacementOrders(topology, {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7},
                          {0, 4, 1, 5, 2, 6, 3, 7}, {0, 1, 2, 3});
}

TEST(TopologyTest, PlacementFillsInterleavedNumaNodesInNodeOrder)
{
    coop::Topology topology{{{0, 1, 0}, {1, 0, 1}, {2, 1, 0}, {3, 0, 1}, {4, 1, 4}, {5, 0, 5}}, {}};
    ExpectPlacementOrders(topology, {0, 1, 2, 3, 4, 5}, {1, 5, 3, 0, 4, 2}, {1, 3, 5, 0, 2, 4},
                          {1, 5, 0, 4});
}

TEST(TopologyTest, PlacementHandlesFilteredUnevenCores)
{
    coop::Topology topology{{{2, 0, 0}, {7, 0, 1}, {9, 0, 1}, {11, 0, 0}, {15, 0, 3}}, {}};
    ExpectPlacementOrders(topology, {2, 7, 9, 11, 15}, {2, 7, 15, 11, 9}, {2, 11, 7, 9, 15},
                          {2, 7, 15});
}

TEST(TopologyTest, PlacementSupportsMoreThanTwoSiblings)
{
    coop::Topology topology{{{0, 0, 0}, {1, 0, 1}, {2, 0, 0}, {3, 0, 1}, {4, 0, 0}, {5, 0, 1}}, {}};
    ExpectPlacementOrders(topology, {0, 1, 2, 3, 4, 5}, {0, 1, 2, 3, 4, 5}, {0, 2, 4, 1, 3, 5},
                          {0, 1});
}

TEST(TopologyTest, PlacementWithoutSmtRetainsEveryCore)
{
    coop::Topology topology{{{2, 0, 2}, {0, 0, 0}, {1, 0, 1}}, {}};
    ExpectPlacementOrders(topology, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2});
}

TEST(TopologyTest, PlacementEmptyAndIncompleteTopology)
{
    ExpectPlacementOrders({}, {}, {}, {}, {});
    coop::Topology topology{{{9, 0, 1}, {2, 0}, {5, 0, 1}}, {}};
    ExpectPlacementOrders(topology, {2, 5, 9}, {2, 5, 9}, {2, 5, 9}, {2, 5, 9});
}

TEST_F(TopologyDiscoveryTest, MissingSysfsSynthesizesNodeAndPreservesCpuOrder)
{
    auto topology = Discover({3, 7});
    ASSERT_EQ(topology.nodes.size(), 1u);
    EXPECT_EQ(topology.nodes[0].node_id, 0);
    EXPECT_EQ(topology.nodes[0].cpus, (std::vector<int>{3, 7}));
    ExpectUnknown(topology);
    ExpectPlacementOrders(topology, {3, 7}, {3, 7}, {3, 7}, {3, 7});
    EXPECT_TRUE(Discover({}).cpus.empty());
    EXPECT_TRUE(Discover({}).nodes.empty());
}

TEST_F(TopologyDiscoveryTest, MissingOrUnreadableSiblingFileDisablesCoreGrouping)
{
    Siblings(0, "0,1\n");
    ExpectUnknown(Discover({0, 1}));
    std::filesystem::create_directories(m_root / "cpu/cpu1/topology/thread_siblings_list");
    ExpectUnknown(Discover({0, 1}));
}

TEST_F(TopologyDiscoveryTest, MalformedAndNonreciprocalSiblingListsDisableCoreGrouping)
{
    Siblings(0, "0,1\n");
    for (char const* list : {"1\n", "0,1x\n", "0\n", "-1,1\n", "0,1,\n", "0,1\n0\n"})
    {
        SCOPED_TRACE(list);
        Siblings(1, list);
        ExpectUnknown(Discover({0, 1}));
    }
    std::string truncated;
    for (int i = 0; i < 3000; ++i)
        truncated += "0,";
    Siblings(1, truncated + "1\n");
    ExpectUnknown(Discover({0, 1}));
}

TEST_F(TopologyDiscoveryTest, OverlapOutsideAffinityMustStillBeConsistent)
{
    Siblings(4, "0,4\n");
    Siblings(5, "0,5\n");
    ExpectUnknown(Discover({4, 5}));
}

TEST_F(TopologyDiscoveryTest, SiblingsCannotSpanNumaNodes)
{
    Write("node/node0/cpulist", "0\n");
    Write("node/node1/cpulist", "1\n");
    Siblings(0, "0,1\n");
    Siblings(1, "0,1\n");
    ExpectUnknown(Discover({0, 1}));
}

TEST_F(TopologyDiscoveryTest, OfflineAndOutsideAffinitySiblingsKeepStableCoreIdentity)
{
    Write("cpu/online", "4-5\n");
    Siblings(4, "0,4,5\n");
    Siblings(5, "0,4,5\n");
    auto topology = Discover({0, 4, 5, 6});
    ASSERT_EQ(topology.cpus.size(), 2u);
    EXPECT_EQ(topology.cpus[0].core_cpu_id, 0);
    EXPECT_EQ(topology.cpus[1].core_cpu_id, 0);
    ExpectPlacementOrders(topology, {4, 5}, {4, 5}, {4, 5}, {4});
    topology = Discover({5});
    ASSERT_EQ(topology.cpus.size(), 1u);
    EXPECT_EQ(topology.cpus[0].core_cpu_id, 0);
    EXPECT_EQ(coop::CpuPlacementOrder(topology, coop::CpuPolicy::NoSmt), (std::vector<int>{5}));
}

TEST_F(TopologyDiscoveryTest, FiltersIntersectAffinityOnlineAndNumaBeforePlacement)
{
    Write("cpu/online", "0-5\n");
    Write("node/node0/cpulist", "0,2,4\n");
    Write("node/node1/cpulist", "1,3,5\n");
    for (int cpu : {0, 2, 4})
        Siblings(cpu, "0,2,4\n");
    for (int cpu : {1, 3, 5})
        Siblings(cpu, "1,3,5\n");
    ScopedEnv cpus("COOP_CPUS", "2-6");
    ScopedEnv node("COOP_NUMA_NODE", "1");
    auto topology = Discover({0, 1, 2, 3, 4, 6});
    ASSERT_EQ(topology.cpus.size(), 1u);
    EXPECT_EQ(topology.cpus[0].cpu_id, 3);
    EXPECT_EQ(topology.cpus[0].core_cpu_id, 1);
    ASSERT_EQ(topology.nodes.size(), 1u);
    EXPECT_EQ(topology.nodes[0].node_id, 1);
    EXPECT_EQ(topology.nodes[0].cpus, (std::vector<int>{3}));
    ExpectPlacementOrders(topology, {3}, {3}, {3}, {3});
}

TEST_F(TopologyDiscoveryTest, CpuEnvironmentPreservesPermissiveGrammar)
{
    for (char const* value : {"0, 2", "  +0,\t+2", ",0,,2,", "0\n2\n",
                             "0suffix,2tail", "junk,0,2", "0-0,2-2suffix",
                             "0-,2", "0 -2,2", "0- 0,2", "0,3-1,2"})
    {
        SCOPED_TRACE(value);
        ScopedEnv cpus("COOP_CPUS", value);
        auto topology = Discover({0, 1, 2, 3});
        EXPECT_EQ(coop::CpuPlacementOrder(topology, coop::CpuPolicy::CpuOrder),
                  (std::vector<int>{0, 2}));
        ASSERT_EQ(topology.nodes.size(), 1u);
        EXPECT_EQ(topology.nodes[0].cpus, (std::vector<int>{0, 2}));
    }
    // The environment parser consumes at most 4095 bytes, including partial tokens.
    //
    std::string truncated = "0" + std::string(4093, ',') + "2,3";
    ScopedEnv cpus("COOP_CPUS", truncated.c_str());
    EXPECT_EQ(coop::CpuPlacementOrder(Discover({0, 1, 2, 3}), coop::CpuPolicy::CpuOrder),
              (std::vector<int>{0, 2}));
}

TEST_F(TopologyDiscoveryTest, NumaEnvironmentPreservesNumericPrefixAndInvalidBehavior)
{
    Write("node/node0/cpulist", "0,2\n");
    Write("node/node1/cpulist", "1,3\n");
    for (char const* value : {"1", " +1", "1suffix", "1,0", "1-0", "1\n0"})
    {
        SCOPED_TRACE(value);
        ScopedEnv node("COOP_NUMA_NODE", value);
        ScopedEnv cpus("COOP_CPUS", "0, 1, 2");
        auto topology = Discover({0, 1, 2, 3});
        EXPECT_EQ(coop::CpuPlacementOrder(topology, coop::CpuPolicy::CpuOrder),
                  (std::vector<int>{1}));
    }
    for (char const* value : {"", "junk", " "})
    {
        ScopedEnv node("COOP_NUMA_NODE", value);
        testing::internal::CaptureStderr();
        auto topology = Discover({0, 1, 2, 3});
        auto diagnostic = testing::internal::GetCapturedStderr();
        EXPECT_EQ(coop::CpuPlacementOrder(topology, coop::CpuPolicy::CpuOrder),
                  (std::vector<int>{0, 1, 2, 3}));
        EXPECT_NE(diagnostic.find("COOP_NUMA_NODE"), std::string::npos);
    }
    for (char const* value : {"-1", "9999"})
    {
        ScopedEnv node("COOP_NUMA_NODE", value);
        testing::internal::CaptureStderr();
        auto topology = Discover({0, 1, 2, 3});
        auto diagnostic = testing::internal::GetCapturedStderr();
        EXPECT_TRUE(topology.cpus.empty());
        EXPECT_TRUE(topology.nodes.empty());
        EXPECT_NE(diagnostic.find("no CPUs"), std::string::npos);
    }
}

TEST_F(TopologyDiscoveryTest, EmptyCpuEnvironmentSelectionsRemainEmptyWithDiagnostic)
{
    for (char const* value : {"", "junk", ",,\n", "3-1", "9999"})
    {
        SCOPED_TRACE(value);
        ScopedEnv cpus("COOP_CPUS", value);
        testing::internal::CaptureStderr();
        auto topology = Discover({0, 1, 2, 3});
        auto diagnostic = testing::internal::GetCapturedStderr();
        EXPECT_TRUE(topology.cpus.empty());
        EXPECT_TRUE(topology.nodes.empty());
        EXPECT_NE(diagnostic.find("no CPUs"), std::string::npos);
        EXPECT_NE(diagnostic.find("automatic pinning"), std::string::npos);
    }
}

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

TEST(TopologyTest, InvalidPlacementPolicyFailsLoudly)
{
    EXPECT_NE(RunWithEnv("TopologySubTest.InvalidPlacementPolicy", {{"COOP_CPU_POLICY", "typo"}}),
              0);
}

TEST(TopologySubTest, InvalidPlacementPolicy)
{
    if (!getenv("COOP_CPU_POLICY"))
        GTEST_SKIP();
    coop::NextRoundRobinCpu();
}

TEST(TopologySubTest, PlacementPolicyCycle)
{
    if (!getenv("COOP_TEST_POLICY_CYCLE"))
        GTEST_SKIP();
    auto order = coop::CpuPlacementOrder(coop::GetTopology(), EnvironmentPolicy());
    if (order.empty())
    {
        EXPECT_EQ(coop::NextRoundRobinCpu(), -1);
        return;
    }
    for (size_t i = 0; i < order.size() * 2 + 1; ++i)
        EXPECT_EQ(coop::NextRoundRobinCpu(), order[i % order.size()]);

    // The next automatic cooperator uses the same cycle, including oversubscription.
    //
    test::RunInCooperator([&](coop::Context* ctx)
                          { EXPECT_EQ(ctx->GetCooperator()->CpuId(), order[1 % order.size()]); });
}

TEST(TopologyTest, EnvPlacementPoliciesCycle)
{
    for (char const* policy : {"cpu-order", "cores-first", "siblings-first", "no-smt"})
    {
        SCOPED_TRACE(policy);
        EXPECT_EQ(
            RunWithEnv("TopologySubTest.PlacementPolicyCycle", {{"COOP_TEST_POLICY_CYCLE", "1"},
                                                                {"COOP_CPU_POLICY", policy},
                                                                {"COOP_NO_PIN", nullptr},
                                                                {"COOP_CPUS", nullptr},
                                                                {"COOP_NUMA_NODE", nullptr}}),
            0);
    }
}

TEST(TopologySubTest, PinningPrecedence)
{
    char const* explicitCpu = getenv("COOP_TEST_EXPLICIT_CPU");
    if (!explicitCpu)
        GTEST_SKIP();
    int cpu = atoi(explicitCpu);
    cpu_set_t inherited;
    ASSERT_EQ(sched_getaffinity(0, sizeof(inherited), &inherited), 0);
    bool disabled = coop::PinningDisabled();

    // An explicit CPU may be outside the automatic assignment allowlist.
    //
    auto config = coop::s_defaultCooperatorConfiguration;
    config.cpuAffinity = cpu;
    coop::Cooperator cooperator(config);
    {
        coop::Thread thread(&cooperator);
        cooperator.Submit(
            [&](coop::Context* ctx)
            {
                cpu_set_t actual;
                EXPECT_EQ(sched_getaffinity(0, sizeof(actual), &actual), 0);
                EXPECT_EQ(ctx->GetCooperator()->CpuId(), disabled ? -1 : cpu);
                if (disabled)
                {
                    EXPECT_EQ(ctx->GetCooperator()->NumaNode(), -1);
                    EXPECT_TRUE(CPU_EQUAL(&actual, &inherited));
                }
                else
                {
                    EXPECT_EQ(CPU_COUNT(&actual), 1);
                    EXPECT_TRUE(CPU_ISSET(cpu, &actual));
                }
                ctx->GetCooperator()->Shutdown();
            });
    }

    // Discovery must retain the original allowed set even when explicit pinning is
    // the runtime's first use. Explicit affinity must not advance the placement counter.
    //
    auto const& topology = coop::GetTopology();
    cpu_set_t expected;
    ASSERT_TRUE(coop::detail::ParseCpuList(getenv("COOP_CPUS"), expected));
    ASSERT_EQ(topology.cpus.size(), static_cast<size_t>(CPU_COUNT(&expected)));
    for (auto const& info : topology.cpus)
        EXPECT_TRUE(CPU_ISSET(info.cpu_id, &expected));
    auto order = coop::CpuPlacementOrder(topology, EnvironmentPolicy());
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(coop::NextRoundRobinCpu(), order[0]);
    if (disabled)
    {
        test::RunInCooperator(
            [&](coop::Context* ctx)
            {
                cpu_set_t actual;
                EXPECT_EQ(sched_getaffinity(0, sizeof(actual), &actual), 0);
                EXPECT_EQ(ctx->GetCooperator()->CpuId(), -1);
                EXPECT_TRUE(CPU_EQUAL(&actual, &inherited));
            });
    }
}

TEST(TopologyTest, EnvNoPinAndExplicitAffinityTakePrecedence)
{
    cpu_set_t available;
    ASSERT_EQ(sched_getaffinity(0, sizeof(available), &available), 0);
    int first = -1, second = -1, last = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (!CPU_ISSET(cpu, &available))
            continue;
        if (first < 0)
            first = cpu;
        else if (second < 0)
            second = cpu;
        last = cpu;
    }
    if (first == last)
        GTEST_SKIP() << "requires two allowed CPUs";
    auto automaticCpu = std::to_string(first);
    if (second != last)
        automaticCpu += "," + std::to_string(second);
    auto explicitCpu = std::to_string(last);
    for (char const* policy : {"cpu-order", "cores-first", "siblings-first", "no-smt"})
    {
        for (char const* noPin : {"0", "1"})
        {
            SCOPED_TRACE(std::string(policy) + "/COOP_NO_PIN=" + noPin);
            EXPECT_EQ(RunWithEnv("TopologySubTest.PinningPrecedence",
                                 {{"COOP_TEST_EXPLICIT_CPU", explicitCpu.c_str()},
                                  {"COOP_CPUS", automaticCpu.c_str()},
                                  {"COOP_NUMA_NODE", nullptr},
                                  {"COOP_CPU_POLICY", policy},
                                  {"COOP_NO_PIN", noPin}}),
                      0);
        }
    }
}

TEST_F(TopologyDiscoveryTest, FilteredOutCpusDoNotRequireSiblingMetadata)
{
    ScopedEnv cpus("COOP_CPUS", "1");
    ScopedEnv numa("COOP_NUMA_NODE", nullptr);
    Siblings(1, "1\n");
    auto topology = Discover({0, 1});
    ASSERT_EQ(topology.cpus.size(), 1u);
    EXPECT_EQ(topology.cpus[0].core_cpu_id, 1);
    ExpectPlacementOrders(topology, {1}, {1}, {1}, {1});
}
