#include <condition_variable>
#include <mutex>
#include <thread>

#include <benchmark/benchmark.h>

// ---------------------------------------------------------------------------
// Uncontended: single thread, fast paths
// ---------------------------------------------------------------------------

// Raw mutex lock + unlock, uncontended. Measures kernel mutex cost.
//
static void BM_Pthread_LockUnlock(benchmark::State& state)
{
    std::mutex mtx;
    for (auto _ : state)
    {
        mtx.lock();
        mtx.unlock();
    }
}
BENCHMARK(BM_Pthread_LockUnlock);

// Lock a mutex, check a pre-satisfied predicate via cv.wait. The predicate is immediately true so
// no actual block occurs. Measures condvar fast-path overhead vs raw mutex.
//
static void BM_Pthread_CondVarSignal(benchmark::State& state)
{
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = true;

    for (auto _ : state)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return ready; });
    }
}
BENCHMARK(BM_Pthread_CondVarSignal);

// cv.wait_for with a large timeout but the predicate is already true so it returns immediately.
// Measures setup overhead of the timed wait path when no actual wait occurs.
//
static void BM_Pthread_CondVar_Timeout_NoFire(benchmark::State& state)
{
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = true;

    for (auto _ : state)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1000), [&] { return ready; });
    }
}
BENCHMARK(BM_Pthread_CondVar_Timeout_NoFire);

// ---------------------------------------------------------------------------
// Contended: two threads, ping-pong
// ---------------------------------------------------------------------------

// Two threads, one mutex, one condvar, integer turn flag. Same two-exchange-per-iteration topology
// as the coop ping-pong benchmarks. Each iteration = 2 round trips.
//
static void BM_Pthread_PingPong(benchmark::State& state)
{
    std::mutex mtx;
    std::condition_variable cv;
    int turn = 0;
    bool done = false;

    std::thread child([&]
    {
        std::unique_lock<std::mutex> lock(mtx);
        while (true)
        {
            cv.wait(lock, [&] { return turn == 1 || done; });
            if (done) break;
            turn = 0;
            cv.notify_one();

            cv.wait(lock, [&] { return turn == 2 || done; });
            if (done) break;
            turn = 0;
            cv.notify_one();
        }
    });

    {
        std::unique_lock<std::mutex> lock(mtx);
        for (auto _ : state)
        {
            turn = 1;
            cv.notify_one();
            cv.wait(lock, [&] { return turn == 0; });

            turn = 2;
            cv.notify_one();
            cv.wait(lock, [&] { return turn == 0; });
        }

        done = true;
        turn = 1;
        cv.notify_one();
    }

    child.join();
}
BENCHMARK(BM_Pthread_PingPong);

// ---------------------------------------------------------------------------
// Thread lifecycle
// ---------------------------------------------------------------------------

// Each iteration: create a thread that blocks on a condvar, signal it to wake, join. Measures the
// full thread create + wake + destroy cycle.
//
static void BM_Pthread_SpawnJoin(benchmark::State& state)
{
    for (auto _ : state)
    {
        std::mutex mtx;
        std::condition_variable cv;
        bool go = false;

        std::thread t([&]
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] { return go; });
        });

        {
            std::lock_guard<std::mutex> lock(mtx);
            go = true;
        }
        cv.notify_one();
        t.join();
    }
}
BENCHMARK(BM_Pthread_SpawnJoin);

// ---------------------------------------------------------------------------
// Timeout fires
// ---------------------------------------------------------------------------

// cv.wait_for with a 1ms timeout that actually fires. Uses 1ms (kernel timer granularity) vs the
// coop benchmark's 50ms (ticker resolution). Both measure "timeout fires and wakes you up" at each
// system's natural resolution.
//
static void BM_Pthread_CondVar_Timeout_Fires(benchmark::State& state)
{
    std::mutex mtx;
    std::condition_variable cv;

    for (auto _ : state)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(1), [] { return false; });
    }
}
BENCHMARK(BM_Pthread_CondVar_Timeout_Fires);
