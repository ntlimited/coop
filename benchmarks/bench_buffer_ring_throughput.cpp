// The missing axis: buffer-ring multishot recv vs classic one-shot recv on THROUGHPUT, not just
// memory.
//
// The buffer-ring memory win is established -- arming N idle keep-alive recvs costs N*bufsize of
// pinned userspace memory the classic way and one shared pool the buffer-ring way. What that work
// left unproven is the per-op cost under actual recv load: does handing buffer selection to the
// kernel and streaming multishot CQEs move more bytes per second, fewer, or the same as the
// caller-owned one-shot path? Memory decoupling would be a poor trade if it taxed throughput.
//
// This benchmark answers it on real sockets. It runs a closed-loop ping-pong echo: a pool of
// driver threads keeps one in-flight request per connection (concurrency == connection count), an
// echo handler on a single cooperator recvs each message and sends it straight back, and the
// driver counts completed round trips over a fixed window. The same load runs two ways:
//
//   classic : each handler owns a private recv buffer and issues a blocking one-shot recv per
//             message (coop's default caller-owned path).
//   armed   : each handler arms one multishot recv that draws from a shared BufferRing pool
//             registered on the cooperator's uring via the Init feature-probe path.
//
// It also reports the idle-armed resident-memory slope for each mode, so the two axes -- the
// memory win and whatever throughput delta accompanies it -- are captured side by side.
//
// Run pinned and warm: taskset -c the process and discard the first post-build run.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"

#include "coop/io/armed_handle.h"
#include "coop/io/buffer_ring.h"
#include "coop/io/descriptor.h"
#include "coop/io/handle.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/io/uring.h"

namespace
{

// One buffer per pool slot. Sized to comfortably hold a single ping so one recv yields one whole
// message and the round-trip count is unambiguous.
//
constexpr int kBufSize  = 2048;
constexpr uint16_t kGroup = 13;

// Pool buffers needed for a saturating closed loop: every connection can have one message in flight
// at once, so the pool must cover the offered concurrency or it exhausts and recvs surface ENOBUFS.
// Sized to the connection count plus headroom (Init rounds up to a power of two). This is the honest
// shape of the buffer-ring throughput comparison: under full saturation the pool tracks peak
// concurrency, so the memory decoupling shows up in the idle-armed phase (keep-alive connections),
// not here.
//
int PoolBufs(int conns) { return conns + 64; }

long RssKib()
{
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long total = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &total, &resident) != 2) resident = -1;
    fclose(f);
    long pageKib = sysconf(_SC_PAGESIZE) / 1024;
    return resident < 0 ? -1 : resident * pageKib;
}

// A connection is a stream socketpair: fds[0] lives in the cooperator (server echo), fds[1] is
// driven by a client thread with plain blocking write/read.
//
struct Conn
{
    int server;
    int client;
};

std::vector<Conn> MakeConns(int n)
{
    std::vector<Conn> conns;
    conns.reserve(n);
    for (int i = 0; i < n; i++)
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(1); }
        conns.push_back({sv[0], sv[1]});
    }
    return conns;
}

bool ReadN(int fd, char* buf, int n)
{
    int got = 0;
    while (got < n)
    {
        ssize_t r = ::read(fd, buf + got, n - got);
        if (r <= 0) return false;
        got += int(r);
    }
    return true;
}

bool WriteN(int fd, const char* buf, int n)
{
    int put = 0;
    while (put < n)
    {
        ssize_t w = ::write(fd, buf + put, n - put);
        if (w <= 0) return false;
        put += int(w);
    }
    return true;
}

// ---- server-side echo handlers (run on the cooperator) ----

// Classic: a blocking one-shot recv into a private buffer, echoed back. The buffer is pinned for
// the connection's whole life -- the memory the buffer ring exists to reclaim.
//
void EchoClassic(coop::Context* ctx, int fd)
{
    coop::io::Descriptor d(coop::io::borrowed, fd, coop::GetUring());
    char buf[kBufSize];
    for (;;)
    {
        int n = coop::io::Recv(d, buf, sizeof(buf));
        if (n <= 0) break;                       // EOF or error: peer closed
        if (coop::io::SendAll(d, buf, n) <= 0) break;
    }
}

// Armed: one multishot recv drawing from the shared pool. Each delivered chunk points into a pool
// buffer (valid until the next Next()), echoed back before that buffer is recycled.
//
void EchoArmed(coop::Context* ctx, int fd, coop::io::BufferRing* br)
{
    coop::io::Descriptor d(coop::io::borrowed, fd, coop::GetUring());
    coop::Coordinator coord;
    coop::io::ArmedHandle ah(ctx, d, br, &coord);
    ah.Arm();
    for (;;)
    {
        coop::io::ArmedHandle::Chunk c;
        int n = ah.Next(&c);
        if (n == -ENOBUFS)
        {
            // The pool drained and the kernel disarmed the multishot. Consumed buffers are
            // recycled by Next(); re-arm and keep serving rather than dropping the connection.
            //
            ah.Arm();
            continue;
        }
        if (n <= 0) break;                       // EOF (0) or a real error (<0)
        if (coop::io::SendAll(d, c.data, n) <= 0) break;
    }
}

struct Result
{
    double   rtPerSec;
    long     rssDeltaKib;
    double   rssPerConnKib;
    int      survivors;
    int      conns;
};

// Idle-armed resident memory slope: arm `conns` recvs each way with no traffic and report the RSS
// the arming alone costs. Runs before the load phase so the client buffers do not contaminate it.
//
long MeasureIdleRss(coop::Context* ctx, bool armed, int conns, coop::io::BufferRing* br)
{
    long base = RssKib();
    auto cs = MakeConns(conns);

    if (!armed)
    {
        std::vector<coop::io::Descriptor*> ds;
        std::vector<coop::Coordinator*> cos;
        std::vector<coop::io::Handle*> hs;
        std::vector<char*> bufs;
        for (auto& c : cs)
        {
            auto* d = new coop::io::Descriptor(coop::io::borrowed, c.server, coop::GetUring());
            auto* co = new coop::Coordinator();
            auto* h = new coop::io::Handle(ctx, *d, co);
            char* b = new char[kBufSize];
            memset(b, 0, kBufSize);              // fault it in as a real handler would
            coop::io::Recv(*h, b, kBufSize);     // arm one-shot recv on idle socket
            ds.push_back(d); cos.push_back(co); hs.push_back(h); bufs.push_back(b);
        }
        coop::GetUring()->Poll();
        long after = RssKib();
        for (auto* h : hs) delete h;
        for (auto* co : cos) delete co;
        for (auto* d : ds) delete d;
        for (auto* b : bufs) delete[] b;
        for (auto& c : cs) { close(c.server); close(c.client); }
        return after - base;
    }

    std::vector<coop::io::Descriptor*> ds;
    std::vector<coop::Coordinator*> cos;
    std::vector<coop::io::ArmedHandle*> hs;
    for (auto& c : cs)
    {
        auto* d = new coop::io::Descriptor(coop::io::borrowed, c.server, coop::GetUring());
        auto* co = new coop::Coordinator();
        auto* h = new coop::io::ArmedHandle(ctx, *d, br, co);
        h->Arm();
        ds.push_back(d); cos.push_back(co); hs.push_back(h);
    }
    coop::GetUring()->Poll();
    long after = RssKib();
    for (auto* h : hs) delete h;
    for (auto* co : cos) delete co;
    for (auto* d : ds) delete d;
    for (auto& c : cs) { close(c.server); close(c.client); }
    return after - base;
}

Result RunMode(bool armed, int conns, int msgSize, int driverThreads, int durationMs)
{
    // Configure the cooperator's uring. The armed run registers a default buffer ring through the
    // Init feature-probe path; both runs widen the rings so many connections can stay in flight.
    //
    coop::CooperatorConfiguration cfg;
    cfg.uring.entries = 4096;
    cfg.uring.registeredSlots = 0;
    if (armed)
    {
        cfg.uring.bufferRingEntries = PoolBufs(conns);
        cfg.uring.bufferRingBufSize = kBufSize;
        cfg.uring.bufferRingGroup = kGroup;
    }

    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    auto conns_v = std::make_shared<std::vector<Conn>>();
    auto rss = std::make_shared<long>(0);

    // Set up on the cooperator thread: measure idle RSS, then make the live connections and spawn
    // an echo handler per connection. Handlers block on recv until the drivers start.
    //
    cooperator.SubmitSync([&](coop::Context* ctx)
    {
        coop::io::BufferRing* br = armed ? coop::GetUring()->GetBufferRing() : nullptr;
        if (armed && !br)
        {
            fprintf(stderr, "buffer ring not registered -- kernel lacks pbuf-ring support?\n");
            exit(1);
        }

        *rss = MeasureIdleRss(ctx, armed, conns, br);

        *conns_v = MakeConns(conns);
        for (auto& c : *conns_v)
        {
            int fd = c.server;
            if (armed)
            {
                coop::Spawn([fd, br](coop::Context* h) { EchoArmed(h, fd, br); });
            }
            else
            {
                coop::Spawn([fd](coop::Context* h) { EchoClassic(h, fd); });
            }
        }
    });

    // A short recv timeout on the client ends turns a missing echo into a bounded read failure
    // rather than an indefinite block, so the load harness always terminates and a lost message
    // costs one connection instead of wedging a whole driver thread.
    //
    struct timeval tv{0, 200 * 1000};   // 200 ms
    for (auto& c : *conns_v) setsockopt(c.client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Closed-loop drivers: each owns a slice of connections and keeps one in-flight ping per live
    // connection (write the whole live slice, then read each echo), counting round trips until the
    // deadline. A connection whose echo fails to arrive is dropped from the live set; surviving
    // connection count is reported as a health signal.
    //
    std::atomic<uint64_t> totalRt{0};
    std::atomic<int> survivors{0};
    std::vector<std::thread> drivers;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    auto t0 = std::chrono::steady_clock::now();

    for (int t = 0; t < driverThreads; t++)
    {
        drivers.emplace_back([&, t]()
        {
            std::vector<char> msg(msgSize, char('a' + t % 26));
            std::vector<char> rbuf(msgSize);
            uint64_t local = 0;
            std::vector<int> live;
            for (int i = t; i < conns; i += driverThreads) live.push_back((*conns_v)[i].client);

            while (!live.empty() && std::chrono::steady_clock::now() < deadline)
            {
                std::vector<int> sent;
                sent.reserve(live.size());
                for (int fd : live) if (WriteN(fd, msg.data(), msgSize)) sent.push_back(fd);

                std::vector<int> stillLive;
                stillLive.reserve(sent.size());
                for (int fd : sent)
                {
                    if (ReadN(fd, rbuf.data(), msgSize)) { local++; stillLive.push_back(fd); }
                }
                live.swap(stillLive);
            }
            totalRt.fetch_add(local, std::memory_order_relaxed);
            survivors.fetch_add(int(live.size()), std::memory_order_relaxed);
        });
    }

    for (auto& d : drivers) d.join();
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    // Close the client ends so the echo handlers see EOF and exit cleanly, then let the cooperator
    // drain a scheduler iteration before shutting down.
    //
    for (auto& c : *conns_v) close(c.client);
    cooperator.SubmitSync([&](coop::Context*) { });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cooperator.Shutdown();
    for (auto& c : *conns_v) close(c.server);

    Result r;
    r.rtPerSec = double(totalRt.load()) / secs;
    r.rssDeltaKib = *rss;
    r.rssPerConnKib = double(*rss) / conns;
    r.survivors = survivors.load();
    r.conns = conns;
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    int conns         = argc > 1 ? atoi(argv[1]) : 500;
    int msgSize       = argc > 2 ? atoi(argv[2]) : 64;
    int driverThreads = argc > 3 ? atoi(argv[3]) : 4;
    int durationMs    = argc > 4 ? atoi(argv[4]) : 3000;

    printf("buffer-ring throughput A/B: conns=%d msg=%dB drivers=%d window=%dms "
           "(pool=%d x %dB)\n",
        conns, msgSize, driverThreads, durationMs, PoolBufs(conns), kBufSize);

    Result classic = RunMode(false, conns, msgSize, driverThreads, durationMs);
    Result armed   = RunMode(true,  conns, msgSize, driverThreads, durationMs);

    printf("\n  classic (one-shot recv, private buffers):\n");
    printf("    throughput : %.0f round-trips/s  (%d/%d connections survived)\n",
        classic.rtPerSec, classic.survivors, classic.conns);
    printf("    idle recv  : +%ld KiB for %d armed (%.3f KiB/conn)\n",
        classic.rssDeltaKib, conns, classic.rssPerConnKib);

    printf("\n  armed (multishot recv, shared pool):\n");
    printf("    throughput : %.0f round-trips/s  (%d/%d connections survived)\n",
        armed.rtPerSec, armed.survivors, armed.conns);
    printf("    idle recv  : +%ld KiB for %d armed (%.3f KiB/conn)\n",
        armed.rssDeltaKib, conns, armed.rssPerConnKib);

    // The idle-recv slope is the marginal cost of arming one more connection (the shared pool is
    // registered once before the slope's baseline snapshot, so it does not appear here -- it is a
    // fixed pool-size cost independent of connection count). Classic scales that slope with every
    // connection; armed's marginal slope collapses toward zero, which is the whole memory point.
    // When armed's marginal RSS rounds to zero a ratio is undefined, so report the slopes directly
    // rather than a misleading "xN".
    //
    double tput = classic.rtPerSec > 0 ? armed.rtPerSec / classic.rtPerSec : 0;
    printf("\n  armed vs classic: throughput x%.2f\n", tput);
    printf("  idle recv slope : classic %.3f KiB/conn  vs  armed %.3f KiB/conn "
           "(armed pool is a fixed %d KiB, conn-independent)\n",
        classic.rssPerConnKib, armed.rssPerConnKib, PoolBufs(conns) * kBufSize / 1024);
    return 0;
}
