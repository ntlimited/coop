#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <vector>

#include <gtest/gtest.h>

#include "coop/io/buffer_ring.h"
#include "coop/self.h"
#include "test_helpers.h"

namespace
{

// Replace the public liburing helper boundaries as well as the workaround's direct calls:
// --wrap cannot intercept calls made inside a shared liburing. The fake ordinary helpers
// model registration ownership; BufferRing's slot publication and cleanup decisions remain
// real. Outside a scoped contract test every wrapper delegates to liburing or the OS.
//
struct BufferRingKernel;
thread_local BufferRingKernel* kernel = nullptr;

struct BufferRingKernel
{
    BufferRingKernel() { kernel = this; }
    ~BufferRingKernel() { kernel = nullptr; }

    const char* release = "6.8.0-139-generic";
    const char* version = "#139-Ubuntu SMP PREEMPT_DYNAMIC";
    bool inverted = true;
    bool unameFails = false;
    bool allocationFails = false;
    int normalError = -EINVAL;
    int retryError = 0;
    int unregisterError = 0;
    int unameCalls = 0;
    int normalUnregisters = 0;
    int normalAllocations = 0;
    int compatAllocationAttempts = 0;
    int compatUnregisters = 0;
    int compatUnmaps = 0;
    std::vector<io_uring_buf_reg> registrations;
    io_uring_buf_reg unregistration{};
    void* mapping = nullptr;
};

#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
constexpr bool kWorkaroundEnabled = true;
#else
constexpr bool kWorkaroundEnabled = false;
#endif

void ExpectUnmapped(void* mapping)
{
    ASSERT_NE(mapping, nullptr);
    unsigned char resident = 0;
    errno = 0;
    EXPECT_EQ(::mincore(mapping, 4096, &resident), -1);
    EXPECT_EQ(errno, ENOMEM);
}

} // namespace

extern "C" int __real_uname(utsname*);
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void*, size_t);
extern "C" int __real_io_uring_register_buf_ring(io_uring*, io_uring_buf_reg*, unsigned);
extern "C" int __real_io_uring_unregister_buf_ring(io_uring*, int);
extern "C" int __real_io_uring_register(unsigned, unsigned, const void*, unsigned);
extern "C" io_uring_buf_ring* __real_io_uring_setup_buf_ring(io_uring*, unsigned, int, unsigned,
                                                            int*);
extern "C" int __real_io_uring_free_buf_ring(io_uring*, io_uring_buf_ring*, unsigned, int);

extern "C" int __wrap_uname(utsname* name)
{
    if (!kernel)
        return __real_uname(name);
    ++kernel->unameCalls;
    if (kernel->unameFails)
    {
        errno = EIO;
        return -1;
    }
    *name = {};
    std::strcpy(name->sysname, "Linux");
    std::strcpy(name->release, kernel->release);
    std::strcpy(name->version, kernel->version);
    return 0;
}

extern "C" void* __wrap_mmap(void* address, size_t size, int prot, int flags, int fd, off_t offset)
{
    if (kernel)
        ++kernel->compatAllocationAttempts;
    if (kernel && kernel->allocationFails)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(address, size, prot, flags, fd, offset);
}

extern "C" int __wrap_munmap(void* address, size_t size)
{
    if (kernel && address == kernel->mapping)
        ++kernel->compatUnmaps;
    return __real_munmap(address, size);
}

extern "C" int __wrap_io_uring_register_buf_ring(io_uring* ring, io_uring_buf_reg* reg,
                                                 unsigned flags)
{
    if (!kernel)
        return __real_io_uring_register_buf_ring(ring, reg, flags);
    kernel->registrations.push_back(*reg);
    kernel->mapping = reinterpret_cast<void*>(reg->ring_addr);
    EXPECT_EQ(flags, 0u);
    EXPECT_EQ(reg->flags, 0u);
    EXPECT_EQ(reg->resv[1], 0u);
    EXPECT_EQ(reg->resv[2], 0u);
    EXPECT_EQ(reg->ring_addr % 4096, 0u);
    if (reg->resv[0] == 0)
        return kernel->inverted ? kernel->normalError : 0;
    EXPECT_EQ(reg->resv[0], 1u);
    EXPECT_GE(reg->ring_entries, 1u);
    EXPECT_LE(reg->ring_entries, 32768u);
    EXPECT_EQ(reg->ring_entries & (reg->ring_entries - 1), 0u);
    return kernel->retryError;
}

extern "C" int __wrap_io_uring_unregister_buf_ring(io_uring* ring, int group)
{
    if (!kernel)
        return __real_io_uring_unregister_buf_ring(ring, group);
    ++kernel->normalUnregisters;
    EXPECT_FALSE(kernel->inverted);
    EXPECT_EQ(group, kernel->registrations.back().bgid);
    return kernel->unregisterError;
}

extern "C" int __wrap_io_uring_register(unsigned fd, unsigned opcode, const void* arg,
                                        unsigned count)
{
    if (!kernel)
        return __real_io_uring_register(fd, opcode, arg, count);
    ++kernel->compatUnregisters;
    EXPECT_EQ(fd, 123u);
    EXPECT_EQ(opcode, unsigned(IORING_UNREGISTER_PBUF_RING));
    EXPECT_EQ(count, 1u);
    kernel->unregistration = *static_cast<const io_uring_buf_reg*>(arg);
    EXPECT_EQ(kernel->unregistration.resv[0], 1u);
    EXPECT_EQ(kernel->unregistration.resv[1], 0u);
    EXPECT_EQ(kernel->unregistration.resv[2], 0u);
    EXPECT_EQ(kernel->unregistration.bgid, kernel->registrations.back().bgid);
    EXPECT_EQ(kernel->unregistration.ring_addr, 0u);
    EXPECT_EQ(kernel->unregistration.ring_entries, 0u);
    EXPECT_EQ(kernel->unregistration.flags, 0u);
    return kernel->unregisterError;
}

extern "C" io_uring_buf_ring* __wrap_io_uring_setup_buf_ring(io_uring* ring, unsigned entries,
                                                            int group, unsigned flags, int* error)
{
    if (!kernel)
        return __real_io_uring_setup_buf_ring(ring, entries, group, flags, error);
    if (entries == 0)
    {
        *error = -EINVAL;
        return nullptr;
    }
    const size_t size = size_t(entries) * sizeof(io_uring_buf);
    ++kernel->normalAllocations;
    // Normal-helper allocation is independent of the workaround allocation-failure injection.
    //
    void* mapping = __real_mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
    {
        *error = -errno;
        return nullptr;
    }
    io_uring_buf_reg reg{};
    reg.ring_addr = reinterpret_cast<uintptr_t>(mapping);
    reg.ring_entries = entries;
    reg.bgid = group;
    *error = __wrap_io_uring_register_buf_ring(ring, &reg, flags);
    if (*error != 0)
    {
        EXPECT_EQ(__real_munmap(mapping, size), 0);
        return nullptr;
    }
    auto* result = static_cast<io_uring_buf_ring*>(mapping);
    io_uring_buf_ring_init(result);
    return result;
}

extern "C" int __wrap_io_uring_free_buf_ring(io_uring* ring, io_uring_buf_ring* mapping,
                                             unsigned entries, int group)
{
    if (!kernel)
        return __real_io_uring_free_buf_ring(ring, mapping, entries, group);
    int result = __wrap_io_uring_unregister_buf_ring(ring, group);
    if (result != 0)
        return result;
    return __real_munmap(mapping, size_t(entries) * sizeof(io_uring_buf)) == 0 ? 0 : -errno;
}

TEST(BufferRingContractTest, HealthyKernelKeepsOrdinaryRegistrationAndCleanup)
{
    BufferRingKernel fake;
    fake.inverted = false;
    coop::io::Uring uring;
    {
        coop::io::BufferRing ring(7, 16, 256);
        ASSERT_EQ(ring.Register(uring), 0);
        ASSERT_EQ(fake.registrations.size(), 1u);
        EXPECT_EQ(fake.registrations[0].resv[0], 0u);
        EXPECT_EQ(fake.unameCalls, 0);
        auto* mapped = static_cast<io_uring_buf_ring*>(fake.mapping);
        EXPECT_EQ(mapped->tail, 16);
        for (unsigned i = 0; i < 16; ++i)
        {
            EXPECT_EQ(mapped->bufs[i].addr, reinterpret_cast<uintptr_t>(ring.Buffer(i)));
            EXPECT_EQ(mapped->bufs[i].len, 256u);
            EXPECT_EQ(mapped->bufs[i].bid, i);
        }
    }
    EXPECT_EQ(fake.normalUnregisters, 1);
    EXPECT_EQ(fake.normalAllocations, 1);
    EXPECT_EQ(fake.compatAllocationAttempts, 0);
    EXPECT_EQ(fake.compatUnregisters, 0);
    ExpectUnmapped(fake.mapping);
}

TEST(BufferRingContractTest, InvertedKernelRequiresBuildOptInAndPairedCleanup)
{
    for (const char* release : {"6.8.0-139-generic", "6.8.0-142-generic"})
    {
        SCOPED_TRACE(release);
        BufferRingKernel fake;
        fake.release = release;
        coop::io::Uring uring;
        uring.Ring()->ring_fd = 123;
        {
            coop::io::BufferRing ring(27, 16, 256);
            ASSERT_EQ(ring.Register(uring), kWorkaroundEnabled ? 0 : -EINVAL);
            ASSERT_EQ(fake.registrations.size(), kWorkaroundEnabled ? 2u : 1u);
            EXPECT_EQ(fake.registrations.front().resv[0], 0u);
            if (kWorkaroundEnabled)
            {
                EXPECT_EQ(fake.registrations.back().resv[0], 1u);
                auto* mapped = static_cast<io_uring_buf_ring*>(fake.mapping);
                EXPECT_EQ(mapped->tail, 16);
                EXPECT_EQ(mapped->bufs[0].addr, reinterpret_cast<uintptr_t>(ring.Buffer(0)));
                ring.ReturnAndPublish(0);
                EXPECT_EQ(mapped->tail, 17);
            }
        }
        EXPECT_EQ(fake.normalUnregisters, 0);
        EXPECT_EQ(fake.normalAllocations, 1);
        EXPECT_EQ(fake.compatAllocationAttempts, kWorkaroundEnabled ? 1 : 0);
        EXPECT_EQ(fake.compatUnregisters, kWorkaroundEnabled ? 1 : 0);
        EXPECT_EQ(fake.compatUnmaps, kWorkaroundEnabled ? 1 : 0);
        ExpectUnmapped(fake.mapping);
    }
}

TEST(BufferRingContractTest, OtherErrorsNeverRetry)
{
    for (int error : {-ENOMEM, -EPERM, -EOPNOTSUPP, -EBUSY})
    {
        SCOPED_TRACE(error);
        BufferRingKernel fake;
        fake.normalError = error;
        coop::io::Uring uring;
        coop::io::BufferRing ring(7, 16, 256);
        EXPECT_EQ(ring.Register(uring), error);
        EXPECT_EQ(fake.registrations.size(), 1u);
        EXPECT_EQ(fake.unameCalls, 0);
        ExpectUnmapped(fake.mapping);
    }
}

TEST(BufferRingContractTest, UnknownKernelNeverRetriesReservedFields)
{
    for (const char* release : {"6.8.0-143-generic", "6.8.0-139-aws", "6.12.0", "6.18.36"})
    {
        SCOPED_TRACE(release);
        BufferRingKernel fake;
        fake.release = release;
        coop::io::Uring uring;
        coop::io::BufferRing ring(7, 16, 256);
        EXPECT_EQ(ring.Register(uring), -EINVAL);
        EXPECT_EQ(fake.registrations.size(), 1u);
        EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 1 : 0);
        ExpectUnmapped(fake.mapping);
    }
}

TEST(BufferRingContractTest, MatchingReleaseWithoutUbuntuIdentityNeverRetries)
{
    BufferRingKernel fake;
    fake.version = "#139 SMP PREEMPT_DYNAMIC";
    coop::io::Uring uring;
    coop::io::BufferRing ring(7, 16, 256);
    EXPECT_EQ(ring.Register(uring), -EINVAL);
    EXPECT_EQ(fake.registrations.size(), 1u);
    EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 1 : 0);
}

TEST(BufferRingContractTest, FailedKernelIdentificationNeverRetries)
{
    BufferRingKernel fake;
    fake.unameFails = true;
    coop::io::Uring uring;
    coop::io::BufferRing ring(7, 16, 256);
    EXPECT_EQ(ring.Register(uring), -EINVAL);
    EXPECT_EQ(fake.registrations.size(), 1u);
    EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 1 : 0);
}

TEST(BufferRingContractDeathTest, OrdinaryUnregisterFailureStopsBeforeFreeingStorage)
{
    EXPECT_EXIT(
        {
            BufferRingKernel fake;
            fake.inverted = false;
            fake.unregisterError = -EIO;
            coop::io::Uring uring;
            coop::io::BufferRing ring(7, 16, 256);
            if (ring.Register(uring) != 0)
                std::_Exit(99);
        },
        ::testing::KilledBySignal(SIGABRT), "");
}

TEST(BufferRingContractTest, InvalidEntriesNeverRetry)
{
    for (unsigned entries : {0u, 3u, 65536u})
    {
        SCOPED_TRACE(entries);
        BufferRingKernel fake;
        coop::io::Uring uring;
        uring.Ring()->ring_fd = 123;
        coop::io::BufferRing ring(7, entries, 1);
        EXPECT_EQ(ring.Register(uring), -EINVAL);
        for (auto const& reg : fake.registrations)
            EXPECT_EQ(reg.resv[0], 0u);
        EXPECT_EQ(fake.compatUnregisters, 0);
    }
}

#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
TEST(BufferRingContractTest, RetryFailureReleasesMapping)
{
    BufferRingKernel fake;
    fake.retryError = -ENOSPC;
    coop::io::Uring uring;
    uring.Ring()->ring_fd = 123;
    coop::io::BufferRing ring(7, 16, 256);
    EXPECT_EQ(ring.Register(uring), -ENOSPC);
    EXPECT_EQ(fake.registrations.size(), 2u);
    EXPECT_EQ(fake.compatUnregisters, 0);
    EXPECT_EQ(fake.compatUnmaps, 1);
    ExpectUnmapped(fake.mapping);
}

TEST(BufferRingContractTest, RetryAllocationFailureRemainsFailure)
{
    BufferRingKernel fake;
    fake.allocationFails = true;
    coop::io::Uring uring;
    uring.Ring()->ring_fd = 123;
    coop::io::BufferRing ring(7, 16, 256);
    EXPECT_EQ(ring.Register(uring), -ENOMEM);
    EXPECT_EQ(fake.normalAllocations, 1);
    EXPECT_EQ(fake.compatAllocationAttempts, 1);
    EXPECT_EQ(fake.unameCalls, 1);
    EXPECT_EQ(fake.registrations.size(), 1u);
    EXPECT_EQ(fake.compatUnregisters, 0);
}

TEST(BufferRingContractDeathTest, FailedUnregisterStopsBeforeReleasingKernelOwnedStorage)
{
    EXPECT_EXIT(
        {
            BufferRingKernel fake;
            fake.unregisterError = -EIO;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            coop::io::BufferRing ring(7, 16, 256);
            if (ring.Register(uring) != 0)
                std::_Exit(99);
        },
        ::testing::KilledBySignal(SIGABRT), "");
}
#endif

TEST(BufferRingTest, RepeatedRegistrationReleasesGroupBeforeReuse)
{
    test::RunInCooperator(
        [](coop::Context*)
        {
            // Queue exit could hide leaked registrations. Reuse the same group on the
            // same live ring, with different pool geometries, to require real cleanup.
            //
            for (unsigned i = 0; i < 64; ++i)
            {
                coop::io::BufferRing ring(53, 1u << (i % 6), 128);
                ASSERT_EQ(ring.Register(*coop::GetUring()), 0) << "iteration " << i;
            }
        });
}
