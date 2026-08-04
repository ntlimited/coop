// Receive-to-disk engine A/B: SpliceToFile vs recv+write bounce.
//
// Motivation (docs/zero_copy_survey_2026-08.md): splice socket -> pipe -> file saves at
// most one copy over a recv+write bounce at identical syscall count — the pipe->file leg
// copies into the page cache and the socket->pipe leg copies the skb linear head. Whether
// the surviving saving is worth the pipe machinery for coop's cooperative IO path is an
// empirical question, and its answer picks the default engine under ReadBodyToFile.
//
// Shape: a producer thread blasts exactly `size` bytes per round over TCP loopback; the
// consumer (a coop context) lands them in a temp file via one of the two engines. Rounds
// alternate engines so ambient load skews both equally. Two metrics per engine and size:
// wall MiB/s, and user+sys CPU seconds per GiB — the second is the load-robust number on
// a shared box, and the one that reflects the copy count the survey argues about.
//
// The file is rewritten from offset 0 each round, so writes land in a warm page cache:
// this deliberately measures the copy path, not device speed — exactly the axis on which
// the engines differ.
//
// Run pinned and in release mode: build/release/bin/bench_disk_path

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/self.h"
#include "coop/thread.h"

#include "coop/io/descriptor.h"
#include "coop/io/pipe_pool.h"
#include "coop/io/recv.h"
#include "coop/io/splice.h"
#include "coop/io/uring.h"
#include "coop/io/write.h"

namespace
{

constexpr size_t kSizes[] = { 4096, 65536, 1048576, 16777216 };
constexpr int    kRounds  = 7;                  // measured, per engine per size, interleaved
constexpr int    kWarmup  = 1;                  // discarded leading rounds per engine per size
constexpr size_t kBounceBuf = 65536;            // matches default pipe capacity

// Small bodies would make a round's byte count too small to measure against rusage
// granularity and ambient noise: each round instead performs enough back-to-back
// transfers of `size` to move at least this much, mirroring how a proxy handles many
// sequential bodies (per-transfer setup cost stays in the measurement).
//
constexpr size_t kMinRoundBytes = 33554432;

size_t ItersFor(size_t size)
{
    return std::max<size_t>(1, kMinRoundBytes / size);
}

const char* SizeLabel(size_t s)
{
    switch (s)
    {
        case 4096:     return "4K";
        case 65536:    return "64K";
        case 1048576:  return "1M";
        case 16777216: return "16M";
    }
    return "?";
}

// Loopback TCP pair — real skb path, unlike AF_UNIX.
//
bool MakeTcpPair(int* clientFd, int* serverFd)
{
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) return false;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listenFd, 1) != 0)
    {
        close(listenFd);
        return false;
    }

    socklen_t len = sizeof(addr);
    getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);

    *clientFd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(*clientFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(listenFd);
        return false;
    }

    *serverFd = accept(listenFd, nullptr, nullptr);
    close(listenFd);
    if (*serverFd < 0) return false;

    int one = 1;
    setsockopt(*clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(*serverFd, F_SETFL, fcntl(*serverFd, F_GETFL) | O_NONBLOCK);
    return true;
}

// Producer: blast the total byte budget. TCP is a stream, so producer chunking need not
// align with consumer transfers; the blocking client socket paces it against the
// consumer. Identical work for both engines.
//
void Produce(int fd, size_t totalBytes)
{
    std::vector<char> chunk(262144);
    for (size_t i = 0; i < chunk.size(); i++)
    {
        chunk[i] = static_cast<char>('a' + (i % 26));
    }

    size_t sent = 0;
    while (sent < totalBytes)
    {
        size_t n = std::min(chunk.size(), totalBytes - sent);
        ssize_t w = ::send(fd, chunk.data(), n, 0);
        if (w <= 0) return;
        sent += static_cast<size_t>(w);
    }
}

bool SpliceRound(coop::io::Descriptor& in, int fileFd, size_t size, coop::io::PipePool& pool)
{
    coop::io::PipeLease pipe(pool);
    if (!pipe) return false;

    size_t remaining = size;
    off_t off = 0;
    while (remaining > 0)
    {
        int n = coop::io::SpliceToFile(in, fileFd, off, pipe.Fds(), remaining);
        if (n <= 0)
        {
            pipe.MarkDirty();
            return false;
        }
        off += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool BounceRound(coop::io::Descriptor& in, coop::io::Descriptor& file, size_t size, char* buf)
{
    size_t remaining = size;
    off_t off = 0;
    while (remaining > 0)
    {
        int n = coop::io::RecvFastpath(in, buf, std::min(kBounceBuf, remaining), 0);
        if (n <= 0) return false;

        int done = 0;
        while (done < n)
        {
            int w = coop::io::Write(file, buf + done, static_cast<size_t>(n - done),
                                    static_cast<uint64_t>(off + done));
            if (w <= 0) return false;
            done += w;
        }
        off += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

double CpuSeconds()
{
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    auto tv = [](const timeval& t) { return t.tv_sec + t.tv_usec / 1e6; };
    return tv(ru.ru_utime) + tv(ru.ru_stime);
}

struct Sample
{
    double wallSec = 0.0;
    double cpuSec  = 0.0;
    size_t bytes   = 0;
};

} // end anonymous namespace

int main()
{
    int clientFd = -1, serverFd = -1;
    if (!MakeTcpPair(&clientFd, &serverFd))
    {
        fprintf(stderr, "loopback setup failed: %s\n", strerror(errno));
        return 1;
    }

    char tmpPath[] = "/tmp/coop_bench_disk_XXXXXX";
    int fileFd = mkstemp(tmpPath);
    if (fileFd < 0)
    {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    unlink(tmpPath);

    // Total byte budget across all sizes, warmup and measured rounds alike
    //
    size_t totalBytes = 0;
    for (size_t size : kSizes)
    {
        totalBytes += (kWarmup + kRounds) * 2 * ItersFor(size) * size;
    }

    std::thread producer(Produce, clientFd, totalBytes);

    // Sample slots: [size][engine]
    //
    Sample samples[std::size(kSizes)][2];

    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);

    cooperator.SubmitSync([&](coop::Context*)
    {
        auto* uring = coop::GetUring();
        coop::io::Descriptor in(serverFd, uring);
        coop::io::Descriptor file(coop::io::borrowed, fileFd, uring);
        std::vector<char> bounceBuf(kBounceBuf);

        for (size_t si = 0; si < std::size(kSizes); si++)
        {
            size_t size = kSizes[si];
            size_t iters = ItersFor(size);

            // Interleaved: alternate splice/bounce per round so ambient load skews both
            // engines equally. Engine index 0 = splice, 1 = bounce. The leading kWarmup
            // pairs are executed but not recorded.
            //
            for (int r = 0; r < (kWarmup + kRounds) * 2; r++)
            {
                int engine = r & 1;
                double cpu0 = CpuSeconds();
                auto t0 = std::chrono::steady_clock::now();

                bool ok = true;
                for (size_t i = 0; ok && i < iters; i++)
                {
                    ok = engine == 0
                        ? SpliceRound(in, fileFd, size, uring->GetPipePool())
                        : BounceRound(in, file, size, bounceBuf.data());
                }

                auto wall = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
                double cpu = CpuSeconds() - cpu0;

                if (!ok)
                {
                    fprintf(stderr, "round failed (size=%zu engine=%d)\n", size, engine);
                    exit(1);
                }

                if (r < kWarmup * 2) continue;

                samples[si][engine].wallSec += wall;
                samples[si][engine].cpuSec  += cpu;
                samples[si][engine].bytes   += iters * size;
            }
        }

        cooperator.Shutdown();
    });

    producer.join();
    close(clientFd);
    close(fileFd);

    printf("\nreceive-to-disk: SpliceToFile vs recv+write bounce "
           "(%d rounds/engine, interleaved, TCP loopback)\n\n", kRounds);
    printf("%-6s %-8s %12s %16s\n", "size", "engine", "MiB/s", "cpu-sec/GiB");
    for (size_t si = 0; si < std::size(kSizes); si++)
    {
        for (int engine = 0; engine < 2; engine++)
        {
            const Sample& s = samples[si][engine];
            double mib = s.bytes / (1024.0 * 1024.0);
            double gib = s.bytes / (1024.0 * 1024.0 * 1024.0);
            printf("%-6s %-8s %12.1f %16.3f\n",
                   SizeLabel(kSizes[si]),
                   engine == 0 ? "splice" : "bounce",
                   mib / s.wallSec,
                   s.cpuSec / gib);
        }
    }
    printf("\ncpu-sec/GiB includes the in-process producer thread (identical work for "
           "both engines).\n");

    return 0;
}
