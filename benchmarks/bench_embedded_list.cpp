#include <algorithm>
#include <memory>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "coop/embedded_list.h"
#include "coop/tricks.h"

// ---------------------------------------------------------------------------
// Node types
// ---------------------------------------------------------------------------

// Minimal node — fits ~2-3 per cache line. Contiguous allocation is very cache-friendly.
//
struct TestNode : coop::EmbeddedListHookups<TestNode>
{
    int value;
};

// Fat node — 256 bytes, spans 4 cache lines. Forces the prefetcher to pull in much more data
// per node during iteration and pointer chasing.
//
struct FatNode : coop::EmbeddedListHookups<FatNode>
{
    int value;
    char padding[256 - sizeof(coop::EmbeddedListHookups<FatNode>) - sizeof(int)];
};

// ---------------------------------------------------------------------------
// Allocation layouts
// ---------------------------------------------------------------------------

// Contiguous: flat array, maximally cache-friendly.
//
struct Contiguous {};

// Scattered: individually heap-allocated and shuffled, defeats spatial locality.
//
struct Scattered {};

// NodePool abstracts over allocation strategy so benchmark bodies stay identical.
//
template<typename Node, typename Layout>
struct NodePool;

template<typename Node>
struct NodePool<Node, Contiguous>
{
    std::vector<Node> nodes;

    NodePool(int n) : nodes(n) {}

    Node* operator[](int i) { return &nodes[i]; }
    int Size() const { return static_cast<int>(nodes.size()); }
};

template<typename Node>
struct NodePool<Node, Scattered>
{
    std::vector<std::unique_ptr<Node>> nodes;

    NodePool(int n)
    {
        nodes.reserve(n);
        for (int i = 0; i < n; i++)
        {
            nodes.push_back(std::make_unique<Node>());
        }
        std::mt19937 rng(42);
        std::shuffle(nodes.begin(), nodes.end(), rng);
    }

    Node* operator[](int i) { return nodes[i].get(); }
    int Size() const { return static_cast<int>(nodes.size()); }
};

// ---------------------------------------------------------------------------
// 1. Push (append to tail)
// ---------------------------------------------------------------------------

template<typename Node, typename Layout>
static void BM_Push(benchmark::State& state)
{
    auto n = state.range(0);
    NodePool<Node, Layout> pool(n);

    for (auto _ : state)
    {
        coop::EmbeddedList<Node> list;
        for (int i = 0; i < n; i++)
        {
            list.Push(pool[i]);
        }
        benchmark::DoNotOptimize(list.IsEmpty());

        state.PauseTiming();
        while (list.Pop()) {}
        state.ResumeTiming();
    }
}
BENCHMARK_TEMPLATE(BM_Push, TestNode, Contiguous)->RangeMultiplier(10)->Range(10, 10000);
BENCHMARK_TEMPLATE(BM_Push, TestNode, Scattered)->RangeMultiplier(10)->Range(10, 10000);

// ---------------------------------------------------------------------------
// 2. Pop (remove from head)
// ---------------------------------------------------------------------------

template<typename Node, typename Layout>
static void BM_Pop(benchmark::State& state)
{
    auto n = state.range(0);
    NodePool<Node, Layout> pool(n);

    for (auto _ : state)
    {
        state.PauseTiming();
        coop::EmbeddedList<Node> list;
        for (int i = 0; i < n; i++)
        {
            list.Push(pool[i]);
        }
        state.ResumeTiming();

        while (auto* out = list.Pop())
        {
            benchmark::DoNotOptimize(out);
        }
    }
}
BENCHMARK_TEMPLATE(BM_Pop, TestNode, Contiguous)->RangeMultiplier(10)->Range(10, 10000);
BENCHMARK_TEMPLATE(BM_Pop, TestNode, Scattered)->RangeMultiplier(10)->Range(10, 10000);

// ---------------------------------------------------------------------------
// 3. Push/Pop churn (FIFO steady-state)
// ---------------------------------------------------------------------------

template<typename Node, typename Layout>
static void BM_PushPopChurn(benchmark::State& state)
{
    constexpr int kSteadySize = 100;
    NodePool<Node, Layout> pool(kSteadySize + 1);

    coop::EmbeddedList<Node> list;
    for (int i = 0; i < kSteadySize; i++)
    {
        list.Push(pool[i]);
    }

    Node* churnNode = pool[kSteadySize];

    for (auto _ : state)
    {
        list.Push(churnNode);
        auto* out = list.Pop();
        benchmark::DoNotOptimize(out);
        churnNode = out;
    }
}
BENCHMARK_TEMPLATE(BM_PushPopChurn, TestNode, Contiguous);
BENCHMARK_TEMPLATE(BM_PushPopChurn, TestNode, Scattered);

// ---------------------------------------------------------------------------
// 4. Iteration — Visit()
// ---------------------------------------------------------------------------

template<typename Node, typename Layout>
static void BM_Visit(benchmark::State& state)
{
    auto n = state.range(0);
    NodePool<Node, Layout> pool(n);

    coop::EmbeddedList<Node> list;
    for (int i = 0; i < n; i++)
    {
        pool[i]->value = i;
        list.Push(pool[i]);
    }

    for (auto _ : state)
    {
        int sum = 0;
        list.Visit([&sum](Node* node) {
            sum += node->value;
            return true;
        });
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK_TEMPLATE(BM_Visit, TestNode, Contiguous)->RangeMultiplier(10)->Range(10, 10000);
BENCHMARK_TEMPLATE(BM_Visit, TestNode, Scattered)->RangeMultiplier(10)->Range(10, 10000);
BENCHMARK_TEMPLATE(BM_Visit, FatNode,  Contiguous)->RangeMultiplier(10)->Range(10, 10000);
BENCHMARK_TEMPLATE(BM_Visit, FatNode,  Scattered)->RangeMultiplier(10)->Range(10, 10000);

// ---------------------------------------------------------------------------
// 5. Iteration — range-for (Iterator)
// ---------------------------------------------------------------------------

template<typename Node, typename Layout>
static void BM_RangeFor(benchmark::State& state)
{
    auto n = state.range(0);
    NodePool<Node, Layout> pool(n);

    coop::EmbeddedList<Node> list;
    for (int i = 0; i < n; i++)
    {
        pool[i]->value = i;
        list.Push(pool[i]);
    }

    for (auto _ : state)
    {
        int sum = 0;
        for (auto* node : list)
        {
            sum += node->value;
        }
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK_TEMPLATE(BM_RangeFor, TestNode, Contiguous)->RangeMultiplier(10)->Range(10, 10000);
BENCHMARK_TEMPLATE(BM_RangeFor, TestNode, Scattered)->RangeMultiplier(10)->Range(10, 10000);

// ---------------------------------------------------------------------------
// 6. Remove from middle
// ---------------------------------------------------------------------------

template<typename Node, typename Layout>
static void BM_RemoveMiddle(benchmark::State& state)
{
    auto n = state.range(0);
    NodePool<Node, Layout> pool(n);

    for (auto _ : state)
    {
        state.PauseTiming();
        coop::EmbeddedList<Node> list;
        for (int i = 0; i < n; i++)
        {
            list.Push(pool[i]);
        }
        state.ResumeTiming();

        list.Remove(pool[n / 2]);
        benchmark::DoNotOptimize(list.IsEmpty());

        state.PauseTiming();
        while (list.Pop()) {}
        state.ResumeTiming();
    }
}
BENCHMARK_TEMPLATE(BM_RemoveMiddle, TestNode, Contiguous)->RangeMultiplier(10)->Range(10, 10000);

// ---------------------------------------------------------------------------
// 7. ascend_cast vs static_cast
// ---------------------------------------------------------------------------

static void BM_AscendCast(benchmark::State& state)
{
    TestNode node;
    node.value = 42;
    coop::EmbeddedListHookups<TestNode>* base = &node;

    for (auto _ : state)
    {
        auto* result = coop::detail::ascend_cast<TestNode>(base);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_AscendCast);

static void BM_StaticCast(benchmark::State& state)
{
    TestNode node;
    node.value = 42;
    coop::EmbeddedListHookups<TestNode>* base = &node;

    for (auto _ : state)
    {
        auto* result = static_cast<TestNode*>(base);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_StaticCast);

BENCHMARK_MAIN();
