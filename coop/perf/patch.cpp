#include "patch.h"

#if COOP_PERF_MODE == 2

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <spdlog/spdlog.h>

namespace coop
{
namespace perf
{

namespace
{

// Layout of each entry in the coop_perf_sites ELF section, emitted by COOP_PERF_INC.
//
struct SectionEntry
{
    uint64_t addr;          // address of the JMP/NOP instruction
    uint32_t counterId;     // Counter enum value
    uint32_t pad;
};

// Saved state per probe site for enable/disable toggling.
//
struct ProbeSite
{
    uint8_t* addr;
    uint8_t  origBytes[5];  // enough for both JEB (2 bytes) and JMP (5 bytes)
    uint8_t  origLen;       // instruction length (2 or 5)
    uint32_t counterId;
};

// Canonical NOP encodings for x86-64.
//
static const uint8_t s_nop2[] = {0x66, 0x90};                          // xchg ax,ax
static const uint8_t s_nop5[] = {0x0f, 0x1f, 0x44, 0x00, 0x00};       // nopl 0(%rax,%rax,1)

// Linker-generated symbols bounding the coop_perf_sites section. Weak so that binaries
// with no probe sites (mode 0/1 builds) still link cleanly.
//
extern "C"
{
    extern char __start_coop_perf_sites[] __attribute__((weak));
    extern char __stop_coop_perf_sites[] __attribute__((weak));
}

static ProbeSite* s_sites = nullptr;
static size_t     s_siteCount = 0;
static bool       s_initialized = false;
static bool       s_enabled = false;

// Patch `len` bytes at `addr`. Makes the containing page(s) writable, writes, restores.
//
static void PatchBytes(uint8_t* addr, const uint8_t* bytes, size_t len)
{
    uintptr_t pageStart = reinterpret_cast<uintptr_t>(addr) & ~0xFFFUL;
    uintptr_t pageEnd   = (reinterpret_cast<uintptr_t>(addr) + len + 0xFFF) & ~0xFFFUL;
    size_t    pageLen    = pageEnd - pageStart;

    mprotect(reinterpret_cast<void*>(pageStart), pageLen, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy(addr, bytes, len);
    mprotect(reinterpret_cast<void*>(pageStart), pageLen, PROT_READ | PROT_EXEC);
}

static void InitSites()
{
    if (s_initialized) return;
    s_initialized = true;

    if (!__start_coop_perf_sites || !__stop_coop_perf_sites)
    {
        SPDLOG_WARN("perf: no probe sites found (section missing)");
        return;
    }

    auto* begin = reinterpret_cast<SectionEntry*>(__start_coop_perf_sites);
    auto* end   = reinterpret_cast<SectionEntry*>(__stop_coop_perf_sites);
    size_t count = end - begin;

    if (count == 0) return;

    // Allocate the site table (one-time, never freed).
    //
    s_sites = new ProbeSite[count];
    s_siteCount = 0;

    for (size_t i = 0; i < count; i++)
    {
        auto* entry = &begin[i];
        auto* addr = reinterpret_cast<uint8_t*>(entry->addr);

        ProbeSite site;
        site.addr = addr;
        site.counterId = entry->counterId;

        // Detect JMP instruction encoding.
        //
        if (addr[0] == 0xEB)
        {
            // JEB rel8 — 2-byte short jump
            //
            site.origLen = 2;
        }
        else if (addr[0] == 0xE9)
        {
            // JMP rel32 — 5-byte near jump
            //
            site.origLen = 5;
        }
        else
        {
            // Unexpected instruction at probe site — skip.
            //
            SPDLOG_WARN("perf: unexpected opcode 0x{:02x} at probe site {:p}, skipping",
                        addr[0], static_cast<void*>(addr));
            continue;
        }

        memcpy(site.origBytes, addr, site.origLen);
        s_sites[s_siteCount++] = site;
    }

    SPDLOG_INFO("perf: initialized {} probe sites", s_siteCount);
}

} // end anonymous namespace

void Enable()
{
    InitSites();
    if (s_enabled) return;

    for (size_t i = 0; i < s_siteCount; i++)
    {
        auto& site = s_sites[i];
        const uint8_t* nop = (site.origLen == 2) ? s_nop2 : s_nop5;
        PatchBytes(site.addr, nop, site.origLen);
    }

    s_enabled = true;
    SPDLOG_INFO("perf: enabled {} probe sites", s_siteCount);
}

void Disable()
{
    if (!s_enabled) return;

    for (size_t i = 0; i < s_siteCount; i++)
    {
        auto& site = s_sites[i];
        PatchBytes(site.addr, site.origBytes, site.origLen);
    }

    s_enabled = false;
    SPDLOG_INFO("perf: disabled {} probe sites", s_siteCount);
}

void Toggle()
{
    if (s_enabled) Disable();
    else Enable();
}

bool IsEnabled()
{
    return s_enabled;
}

size_t ProbeCount()
{
    InitSites();
    return s_siteCount;
}

} // end namespace coop::perf
} // end namespace coop

#endif // COOP_PERF_MODE == 2
