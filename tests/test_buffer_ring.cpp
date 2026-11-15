#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/io/buffer_ring.h"
#include "coop/self.h"
#include "test_helpers.h"

namespace
{

// Replace public liburing helper boundaries too: --wrap cannot intercept calls inside a shared
// liburing. Only ABI acceptance and mapping ownership are faked; slot publication remains real.
// Each incompatible ABI scenario re-execs in a fresh process, so no production cache reset is
// needed.
//
struct BufferRingKernel;
thread_local BufferRingKernel* kernel = nullptr;
constexpr unsigned kConcurrentRegistrations = 4;

struct BufferRingKernel
{
    struct Registration
    {
        io_uring* ring;
        uint16_t group;
        void* mapping;
    };

    BufferRingKernel() { kernel = this; }
    ~BufferRingKernel()
    {
        EXPECT_TRUE(active.empty());
        kernel = nullptr;
    }

    bool inverted = true;
    const char* release = "6.8.0-149-generic";
    const char* version = "#149-Ubuntu SMP PREEMPT_DYNAMIC";
    bool unameFails = false;
    bool allocationFails = false;
    int normalError = 0;
    int retryError = 0;
    int unregisterError = 0;
    int unameCalls = 0;
    int normalUnregisters = 0;
    int normalAllocations = 0;
    int compatAllocationAttempts = 0;
    int compatUnregisters = 0;
    int compatUnmaps = 0;
    std::atomic<unsigned>* firstNormalArrivals = nullptr;
    std::vector<io_uring_buf_reg> registrations;
    std::vector<Registration> active;
    void* mapping = nullptr;

    int Unregister(io_uring* ring, uint16_t group)
    {
        if (unregisterError)
            return unregisterError;
        auto found = std::find_if(active.begin(), active.end(),
                                  [&](Registration const& reg)
                                  { return (!ring || reg.ring == ring) && reg.group == group; });
        if (found == active.end())
            return -ENOENT;
        active.erase(found);
        return 0;
    }
};

#if defined(COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK) &&                                         \
    COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK
constexpr bool kWorkaroundEnabled = true;
#else
constexpr bool kWorkaroundEnabled = false;
#endif

template<typename Fn>
void InFreshProcess(Fn fn)
{
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    EXPECT_EXIT(
        {
            fn();
            std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "");
}

void ExpectUnmapped(void* mapping)
{
    ASSERT_NE(mapping, nullptr);
    unsigned char resident = 0;
    errno = 0;
    EXPECT_EQ(::mincore(mapping, 4096, &resident), -1);
    EXPECT_EQ(errno, ENOMEM);
}

void ExpectSeeded(coop::io::BufferRing& ring, void* mapping)
{
    auto* mapped = static_cast<io_uring_buf_ring*>(mapping);
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mapped->tail, 16);
    for (unsigned i = 0; i < 16; ++i)
    {
        EXPECT_EQ(mapped->bufs[i].addr, reinterpret_cast<uintptr_t>(ring.Buffer(i)));
        EXPECT_EQ(mapped->bufs[i].len, 256u);
        EXPECT_EQ(mapped->bufs[i].bid, i);
    }
    ring.ReturnAndPublish(0);
    EXPECT_EQ(mapped->tail, 17);
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
    if (kernel)
    {
        // Unmapping a registered ring, including after a failed unregister, violates ownership.
        // A distinct exit code makes the teardown death tests reject this failure mode.
        //
        for (auto const& reg : kernel->active)
            if (reg.mapping == address)
                std::_Exit(98);
        if (address == kernel->mapping)
            ++kernel->compatUnmaps;
    }
    return __real_munmap(address, size);
}

extern "C" int __wrap_io_uring_register_buf_ring(io_uring* ring, io_uring_buf_reg* reg,
                                                 unsigned flags)
{
    if (!kernel)
        return __real_io_uring_register_buf_ring(ring, reg, flags);
    if (reg->resv[0] == 0 && kernel->firstNormalArrivals)
    {
        // Hold every initial standard attempt before any can publish a successful ABI choice.
        // Subsequent registrations on this fake must proceed without waiting again.
        //
        auto* arrivals = kernel->firstNormalArrivals;
        kernel->firstNormalArrivals = nullptr;
        arrivals->fetch_add(1, std::memory_order_acq_rel);
        while (arrivals->load(std::memory_order_acquire) != kConcurrentRegistrations)
            std::this_thread::yield();
    }
    kernel->registrations.push_back(*reg);
    kernel->mapping = reinterpret_cast<void*>(reg->ring_addr);
    EXPECT_EQ(flags, 0u);
    EXPECT_EQ(reg->flags, 0u);
    EXPECT_EQ(reg->resv[1], 0u);
    EXPECT_EQ(reg->resv[2], 0u);
    EXPECT_LE(reg->resv[0], 1u);
    if (ring->ring_fd < 0)
        return -EBADF;
    if (reg->resv[0] == 0 && kernel->normalError)
        return kernel->normalError;
    if ((reg->resv[0] != 0) != kernel->inverted)
        return -EINVAL;
    if (!reg->ring_addr || reg->ring_addr % 4096 != 0 || reg->ring_entries == 0 ||
        reg->ring_entries > 32768 || (reg->ring_entries & (reg->ring_entries - 1)) != 0)
        return -EINVAL;
    for (auto const& active : kernel->active)
        if (active.ring == ring && active.group == reg->bgid)
            return -EEXIST;
    if (reg->resv[0] != 0 && kernel->retryError)
        return kernel->retryError;
    kernel->active.push_back({ring, reg->bgid, kernel->mapping});
    return 0;
}

extern "C" int __wrap_io_uring_unregister_buf_ring(io_uring* ring, int group)
{
    if (!kernel)
        return __real_io_uring_unregister_buf_ring(ring, group);
    ++kernel->normalUnregisters;
    if (kernel->inverted)
        return -EINVAL;
    return kernel->Unregister(ring, group);
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
    auto const& reg = *static_cast<const io_uring_buf_reg*>(arg);
    EXPECT_EQ(reg.resv[0], 1u);
    EXPECT_EQ(reg.resv[1], 0u);
    EXPECT_EQ(reg.resv[2], 0u);
    EXPECT_EQ(reg.ring_addr, 0u);
    EXPECT_EQ(reg.ring_entries, 0u);
    EXPECT_EQ(reg.flags, 0u);
    if (!kernel->inverted)
        return -EINVAL;
    return kernel->Unregister(nullptr, reg.bgid);
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
    // Ordinary-helper allocation is independent of compatibility allocation failure injection.
    //
    void* mapping =
        __real_mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

TEST(BufferRingContractTest, HealthyKernelUsesOneOrdinaryRegistrationPerRing)
{
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            fake.inverted = false;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            for (unsigned i = 0; i < 3; ++i)
            {
                {
                    coop::io::BufferRing ring(7, 16, 256);
                    ASSERT_EQ(ring.Register(uring), 0);
                    ASSERT_EQ(fake.registrations.size(), i + 1);
                    EXPECT_EQ(fake.registrations.back().resv[0], 0u);
                    ExpectSeeded(ring, fake.mapping);
                }
                ExpectUnmapped(fake.mapping);
            }
            EXPECT_EQ(fake.normalUnregisters, 3);
            EXPECT_EQ(fake.normalAllocations, 3);
            EXPECT_EQ(fake.compatAllocationAttempts, 0);
            EXPECT_EQ(fake.compatUnregisters, 0);
            EXPECT_EQ(fake.unameCalls, 0);
        });
}

TEST(BufferRingContractTest, InvertedKernelLearnsAcceptedAbiAndPairsCleanup)
{
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            for (unsigned i = 0; i < 3; ++i)
            {
                {
                    coop::io::BufferRing ring(27, 16, 256);
                    ASSERT_EQ(ring.Register(uring), kWorkaroundEnabled ? 0 : -EINVAL);
                    ASSERT_EQ(fake.registrations.size(), kWorkaroundEnabled ? i + 2 : i + 1);
                    EXPECT_EQ(fake.registrations.front().resv[0], 0u);
                    if (kWorkaroundEnabled)
                    {
                        EXPECT_EQ(fake.registrations.back().resv[0], 1u);
                        ExpectSeeded(ring, fake.mapping);
                    }
                }
                ExpectUnmapped(fake.mapping);
            }
            EXPECT_EQ(fake.normalAllocations, kWorkaroundEnabled ? 1 : 3);
            EXPECT_EQ(fake.compatAllocationAttempts, kWorkaroundEnabled ? 3 : 0);
            EXPECT_EQ(fake.compatUnregisters, kWorkaroundEnabled ? 3 : 0);
            EXPECT_EQ(fake.compatUnmaps, kWorkaroundEnabled ? 3 : 0);
            EXPECT_EQ(fake.normalUnregisters, 0);
            EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 1 : 0);
        });
}

TEST(BufferRingContractTest, UnrelatedKernelFamilyNeverRetriesOrCachesReservedFields)
{
    for (const char* release : {"6.8.0", "6.8.1-149-generic", "6.9.0-149-generic", "6.18.36"})
    {
        InFreshProcess(
            [release]
            {
                BufferRingKernel fake;
                fake.release = release;
                coop::io::Uring uring;
                uring.Ring()->ring_fd = 123;
                for (unsigned i = 0; i < 2; ++i)
                {
                    coop::io::BufferRing ring(7, 16, 256);
                    EXPECT_EQ(ring.Register(uring), -EINVAL);
                }
                EXPECT_EQ(fake.registrations.size(), 2u);
                EXPECT_EQ(fake.compatAllocationAttempts, 0);
                EXPECT_TRUE(fake.active.empty());
                EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 2 : 0);
                // Rejection must not classify the ABI: a later eligible request still probes.
                //
                fake.release = "6.8.0-149-generic";
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), kWorkaroundEnabled ? 0 : -EINVAL);
                EXPECT_EQ(fake.registrations.size(), kWorkaroundEnabled ? 4u : 3u);
            });
    }
}

TEST(BufferRingContractTest, MatchingReleaseWithoutUbuntuIdentityNeverRetries)
{
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            fake.version = "#149 SMP PREEMPT_DYNAMIC";
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            {
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), -EINVAL);
            }
            EXPECT_EQ(fake.registrations.size(), 1u);
            EXPECT_EQ(fake.compatAllocationAttempts, 0);
            EXPECT_TRUE(fake.active.empty());
            fake.inverted = false;
            {
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), 0);
                EXPECT_EQ(fake.registrations.back().resv[0], 0u);
            }
            EXPECT_EQ(fake.registrations.size(), 2u);
            EXPECT_EQ(fake.normalUnregisters, 1);
            EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 1 : 0);
        });
}

TEST(BufferRingContractTest, FailedKernelIdentificationPreservesErrorAndDiscovery)
{
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            fake.unameFails = true;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            {
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), -EINVAL);
            }
            EXPECT_EQ(fake.registrations.size(), 1u);
            EXPECT_EQ(fake.compatAllocationAttempts, 0);
            EXPECT_TRUE(fake.active.empty());
            fake.unameFails = false;
            for (unsigned i = 0; i < 2; ++i)
            {
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), kWorkaroundEnabled ? 0 : -EINVAL);
            }
            EXPECT_EQ(fake.registrations.size(), kWorkaroundEnabled ? 4u : 3u);
            EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 2 : 0);
        });
}

TEST(BufferRingContractTest, OtherErrorsNeverRetryOrPoisonLaterDiscovery)
{
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            for (int error : {-ENOMEM, -EPERM, -EOPNOTSUPP, -EBUSY, -EACCES, -EBADF, -EEXIST})
            {
                SCOPED_TRACE(error);
                fake.normalError = error;
                size_t attempts = fake.registrations.size();
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), error);
                EXPECT_EQ(fake.registrations.size(), attempts + 1);
                EXPECT_EQ(fake.compatAllocationAttempts, 0);
                ExpectUnmapped(fake.mapping);
            }
            fake.normalError = 0;
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), kWorkaroundEnabled ? 0 : -EINVAL);
            }
            EXPECT_EQ(fake.compatUnregisters, kWorkaroundEnabled ? 1 : 0);
            EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 1 : 0);
        });
}

TEST(BufferRingContractTest, GenuineInvalidArgumentDoesNotSelectCompatibilityAbi)
{
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            fake.inverted = false;
            fake.normalError = -EINVAL;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            {
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), -EINVAL);
            }
            ExpectUnmapped(fake.mapping);
            fake.normalError = 0;
            for (unsigned i = 0; i < 2; ++i)
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), 0);
                EXPECT_EQ(fake.registrations.back().resv[0], 0u);
            }
            EXPECT_EQ(fake.registrations.size(), kWorkaroundEnabled ? 4u : 3u);
            EXPECT_EQ(fake.compatUnregisters, 0);
            EXPECT_EQ(fake.normalUnregisters, 2);
            EXPECT_EQ(fake.unameCalls, kWorkaroundEnabled ? 1 : 0);
        });
}

TEST(BufferRingContractTest, RetryFailureReleasesMappingAndDoesNotPoisonDiscovery)
{
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            fake.retryError = -ENOSPC;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            {
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), kWorkaroundEnabled ? -ENOSPC : -EINVAL);
            }
            EXPECT_EQ(fake.compatUnmaps, kWorkaroundEnabled ? 1 : 0);
            EXPECT_TRUE(fake.active.empty());
            ExpectUnmapped(fake.mapping);
            fake.retryError = 0;
            for (unsigned i = 0; i < 2; ++i)
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), kWorkaroundEnabled ? 0 : -EINVAL);
            }
            EXPECT_EQ(fake.registrations.size(), kWorkaroundEnabled ? 5u : 3u);
            EXPECT_EQ(fake.normalAllocations, kWorkaroundEnabled ? 2 : 3);
            EXPECT_EQ(fake.compatUnregisters, kWorkaroundEnabled ? 2 : 0);
        });
}

TEST(BufferRingContractTest, RetryAllocationFailureDoesNotPoisonDiscovery)
{
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            fake.allocationFails = true;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            {
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), kWorkaroundEnabled ? -ENOMEM : -EINVAL);
            }
            EXPECT_EQ(fake.registrations.size(), 1u);
            EXPECT_EQ(fake.compatAllocationAttempts, kWorkaroundEnabled ? 1 : 0);
            EXPECT_TRUE(fake.active.empty());
            ExpectUnmapped(fake.mapping);
            fake.allocationFails = false;
            for (unsigned i = 0; i < 2; ++i)
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), kWorkaroundEnabled ? 0 : -EINVAL);
            }
            EXPECT_EQ(fake.registrations.size(), kWorkaroundEnabled ? 4u : 3u);
            EXPECT_EQ(fake.compatAllocationAttempts, kWorkaroundEnabled ? 3 : 0);
            EXPECT_EQ(fake.compatUnregisters, kWorkaroundEnabled ? 2 : 0);
        });
}

TEST(BufferRingContractTest, InvalidArgumentsNeverUseAlternateAbiBeforeOrAfterDiscovery)
{
    for (bool classified : {false, true})
    {
        InFreshProcess(
            [classified]
            {
                BufferRingKernel fake;
                coop::io::Uring uring;
                uring.Ring()->ring_fd = 123;
                if (classified)
                {
                    coop::io::BufferRing ring(7, 16, 256);
                    ASSERT_EQ(ring.Register(uring), kWorkaroundEnabled ? 0 : -EINVAL);
                }
                const int allocations = fake.compatAllocationAttempts;
                const size_t attempts = fake.registrations.size();
                for (unsigned entries : {0u, 3u, 65536u})
                {
                    SCOPED_TRACE(entries);
                    coop::io::BufferRing ring(7, entries, 1);
                    EXPECT_EQ(ring.Register(uring), -EINVAL);
                }
                {
                    coop::io::BufferRing ring(7, 16, 0);
                    EXPECT_EQ(ring.Register(uring), -EINVAL);
                }
                uring.Ring()->ring_fd = -1;
                {
                    coop::io::BufferRing ring(7, 16, 256);
                    EXPECT_EQ(ring.Register(uring), -EBADF);
                }
                EXPECT_EQ(fake.compatAllocationAttempts, allocations);
                for (size_t i = attempts; i < fake.registrations.size(); ++i)
                    EXPECT_EQ(fake.registrations[i].resv[0], 0u);
                EXPECT_EQ(fake.unameCalls, classified && kWorkaroundEnabled ? 1 : 0);
            });
    }
}

TEST(BufferRingContractTest, DuplicateGroupCannotBeMaskedByEitherAbi)
{
    for (bool inverted : {false, true})
    {
        InFreshProcess(
            [inverted]
            {
                BufferRingKernel fake;
                fake.inverted = inverted;
                coop::io::Uring uring;
                uring.Ring()->ring_fd = 123;
                {
                    coop::io::BufferRing owner(7, 16, 256);
                    ASSERT_EQ(owner.Register(uring), inverted && !kWorkaroundEnabled ? -EINVAL : 0);
                    if (inverted && !kWorkaroundEnabled)
                        return;
                    void* owned = fake.mapping;
                    {
                        coop::io::BufferRing duplicate(7, 16, 256);
                        EXPECT_EQ(duplicate.Register(uring), -EEXIST);
                    }
                    ASSERT_EQ(fake.active.size(), 1u);
                    EXPECT_EQ(fake.active.front().mapping, owned);
                    ExpectSeeded(owner, owned);
                }
                EXPECT_TRUE(fake.active.empty());
                coop::io::BufferRing replacement(7, 16, 256);
                EXPECT_EQ(replacement.Register(uring), 0);
            });
    }
}

TEST(BufferRingContractTest, CachedAbiStillReportsPerRingFailures)
{
    for (bool inverted : {false, true})
    {
        InFreshProcess(
            [inverted]
            {
                BufferRingKernel fake;
                fake.inverted = inverted;
                coop::io::Uring uring;
                uring.Ring()->ring_fd = 123;
                {
                    coop::io::BufferRing ring(7, 16, 256);
                    ASSERT_EQ(ring.Register(uring), inverted && !kWorkaroundEnabled ? -EINVAL : 0);
                }
                if (inverted && !kWorkaroundEnabled)
                    return;
                const size_t classifiedAttempts = fake.registrations.size();
                for (int error : {-EINVAL, -ENOMEM, -EACCES})
                {
                    if (inverted)
                        fake.retryError = error;
                    else
                        fake.normalError = error;
                    coop::io::BufferRing ring(7, 16, 256);
                    EXPECT_EQ(ring.Register(uring), error);
                    EXPECT_TRUE(fake.active.empty());
                }
                fake.normalError = fake.retryError = 0;
                {
                    coop::io::BufferRing ring(7, 16, 256);
                    EXPECT_EQ(ring.Register(uring), 0);
                }
                EXPECT_EQ(fake.registrations.size(), classifiedAttempts + (inverted ? 5 : 4));
                EXPECT_EQ(fake.registrations.back().resv[0], inverted ? 1u : 0u);
            });
    }
}

TEST(BufferRingContractTest, CachedNonzeroEinvalRecoversStandardAbi)
{
    if (!kWorkaroundEnabled)
        GTEST_SKIP() << "Compatibility registration is disabled in this build";
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), 0);
            }
            ASSERT_EQ(fake.registrations.size(), 2u);
            ASSERT_EQ(fake.compatUnregisters, 1);
            // Retire the old ring before simulating the live fix. Existing rings retain their
            // own paired unregister encoding; this test exercises only new registration.
            //
            fake.inverted = false;
            fake.release = "6.9.0-1-generic";
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), 0);
                ASSERT_EQ(fake.registrations.size(), 4u);
                EXPECT_EQ(fake.registrations[2].resv[0], 1u);
                EXPECT_EQ(fake.registrations[3].resv[0], 0u);
                ExpectSeeded(ring, fake.mapping);
            }
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), 0);
                ASSERT_EQ(fake.registrations.size(), 5u);
                EXPECT_EQ(fake.registrations.back().resv[0], 0u);
            }
            EXPECT_EQ(fake.normalUnregisters, 2);
            EXPECT_EQ(fake.compatUnregisters, 1);
            EXPECT_EQ(fake.compatAllocationAttempts, 2);
            EXPECT_EQ(fake.unameCalls, 1);
        });
}

TEST(BufferRingContractTest, FailedStandardRecoveryDoesNotClassifyTheAbi)
{
    if (!kWorkaroundEnabled)
        GTEST_SKIP() << "Compatibility registration is disabled in this build";
    InFreshProcess(
        []
        {
            BufferRingKernel fake;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), 0);
            }
            fake.inverted = false;
            fake.normalError = -EPERM;
            {
                coop::io::BufferRing ring(7, 16, 256);
                EXPECT_EQ(ring.Register(uring), -EPERM);
            }
            EXPECT_TRUE(fake.active.empty());
            EXPECT_EQ(fake.registrations.size(), 4u);
            fake.normalError = 0;
            {
                coop::io::BufferRing ring(7, 16, 256);
                ASSERT_EQ(ring.Register(uring), 0);
                ASSERT_EQ(fake.registrations.size(), 6u);
                EXPECT_EQ(fake.registrations[4].resv[0], 1u);
                EXPECT_EQ(fake.registrations[5].resv[0], 0u);
            }
            EXPECT_EQ(fake.unameCalls, 1);
            EXPECT_EQ(fake.normalUnregisters, 1);
            EXPECT_EQ(fake.compatUnregisters, 1);
        });
}

TEST(BufferRingContractTest, ConcurrentFirstRegistrationsRetainIndependentOwnership)
{
    for (bool inverted : {false, true})
    {
        InFreshProcess(
            [inverted]
            {
                std::atomic<unsigned> ready{0};
                std::atomic<unsigned> firstNormalArrivals{0};
                std::atomic<bool> start{false};
                std::thread threads[kConcurrentRegistrations];
                for (auto& thread : threads)
                {
                    thread = std::thread(
                        [&]
                        {
                            BufferRingKernel fake;
                            fake.inverted = inverted;
                            fake.firstNormalArrivals = &firstNormalArrivals;
                            coop::io::Uring uring;
                            uring.Ring()->ring_fd = 123;
                            ready.fetch_add(1, std::memory_order_release);
                            while (!start.load(std::memory_order_acquire))
                                std::this_thread::yield();
                            {
                                coop::io::BufferRing ring(7, 16, 256);
                                ASSERT_EQ(ring.Register(uring),
                                          inverted && !kWorkaroundEnabled ? -EINVAL : 0);
                                if (!inverted || kWorkaroundEnabled)
                                    ExpectSeeded(ring, fake.mapping);
                            }
                            const size_t firstAttempts = fake.registrations.size();
                            EXPECT_EQ(firstAttempts, inverted && kWorkaroundEnabled ? 2u : 1u);
                            {
                                coop::io::BufferRing ring(7, 16, 256);
                                ASSERT_EQ(ring.Register(uring),
                                          inverted && !kWorkaroundEnabled ? -EINVAL : 0);
                            }
                            EXPECT_EQ(fake.registrations.size(), firstAttempts + 1);
                            EXPECT_EQ(fake.unameCalls, inverted && kWorkaroundEnabled ? 1 : 0);
                            EXPECT_TRUE(fake.active.empty());
                        });
                }
                while (ready.load(std::memory_order_acquire) != kConcurrentRegistrations)
                    std::this_thread::yield();
                start.store(true, std::memory_order_release);
                for (auto& thread : threads)
                    thread.join();
                EXPECT_EQ(firstNormalArrivals.load(std::memory_order_acquire),
                          kConcurrentRegistrations);
            });
    }
}

TEST(BufferRingContractDeathTest, OrdinaryUnregisterFailureStopsBeforeFreeingStorage)
{
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    EXPECT_EXIT(
        {
            BufferRingKernel fake;
            fake.inverted = false;
            fake.unregisterError = -EIO;
            coop::io::Uring uring;
            uring.Ring()->ring_fd = 123;
            coop::io::BufferRing ring(7, 16, 256);
            if (ring.Register(uring) != 0)
                std::_Exit(99);
        },
        ::testing::KilledBySignal(SIGABRT), "");
}

TEST(BufferRingContractDeathTest, CompatibilityUnregisterFailureStopsBeforeReleasingStorage)
{
    if (!kWorkaroundEnabled)
        GTEST_SKIP() << "Compatibility registration is disabled in this build";
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
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

TEST(BufferRingTest, InvalidGeometryAndDuplicateGroupsRemainErrors)
{
    test::RunInCooperator(
        [](coop::Context*)
        {
            auto& uring = *coop::GetUring();
            for (unsigned entries : {0u, 3u, 65536u})
            {
                SCOPED_TRACE(entries);
                coop::io::BufferRing invalid(91, entries, 1);
                EXPECT_EQ(invalid.Register(uring), -EINVAL);
            }

            coop::io::BufferRing owner(91, 16, 256);
            ASSERT_EQ(owner.Register(uring), 0);
            coop::io::BufferRing duplicate(91, 16, 256);
            EXPECT_EQ(duplicate.Register(uring), -EEXIST);
        });
}
