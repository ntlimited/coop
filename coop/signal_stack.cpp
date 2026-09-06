#include "signal_stack.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace coop
{

namespace
{

// A floor under whatever the platform asks for.
//
// sysconf(_SC_SIGSTKSZ) covers the kernel's frame and glibc's own preamble; it says nothing about
// the handler body, and coop's handlers do real work — the sampler walks frames and writes into a
// ring, a crash handler may symbolize. Sanitizer runtimes install alternate stacks of their own and
// need considerably more than the bare figure when they report. This is cheap headroom: one mapping
// per thread that runs cooperators, and threads that run cooperators are counted in single digits.
//
constexpr size_t kFloor = 128 * 1024;

size_t PageSize()
{
    static size_t const cached = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return cached;
}

} // end anonymous namespace

size_t SignalStack::PreferredSize()
{
    size_t wanted = 0;

#ifdef _SC_SIGSTKSZ
    long const fromSysconf = sysconf(_SC_SIGSTKSZ);
    if (fromSysconf > 0)
    {
        wanted = static_cast<size_t>(fromSysconf);
    }
#endif

    // SIGSTKSZ is itself the sysconf call on a modern glibc and the legacy constant on an older one,
    // so it is the right fallback either way — it is only unusable as a compile-time value.
    //
    if (wanted == 0)
    {
        wanted = static_cast<size_t>(SIGSTKSZ);
    }

    if (wanted < static_cast<size_t>(MINSIGSTKSZ))
    {
        wanted = static_cast<size_t>(MINSIGSTKSZ);
    }

    if (wanted < kFloor)
    {
        wanted = kFloor;
    }

    size_t const page = PageSize();
    return (wanted + page - 1) & ~(page - 1);
}

void const* SignalStack::Bottom() const
{
    return m_memory ? static_cast<uint8_t const*>(m_memory) + PageSize() : nullptr;
}

bool SignalStack::IsActive()
{
    stack_t current{};
    if (sigaltstack(nullptr, &current) != 0)
    {
        return false;
    }
    return (current.ss_flags & SS_DISABLE) == 0 && current.ss_sp != nullptr;
}

SignalStack::SignalStack()
: m_memory(nullptr)
, m_size(0)
, m_previous{}
, m_installed(false)
{
    size_t const wanted = PreferredSize();

    if (sigaltstack(nullptr, &m_previous) != 0)
    {
        m_previous = stack_t{};
        m_previous.ss_flags = SS_DISABLE;
    }

    // Something adequate is already here — a sanitizer runtime's, or the host's. Replacing it would
    // be a downgrade for whoever installed it and a surprise for whoever relies on its size.
    //
    if ((m_previous.ss_flags & SS_DISABLE) == 0 && m_previous.ss_sp != nullptr
        && m_previous.ss_size >= wanted)
    {
        return;
    }

    // A guard page below the stack, for the same reason context segments have one: an alternate
    // stack that overflows silently corrupts whatever is mapped beneath it, and the whole point of
    // being here is that the overflow already happened once somewhere else.
    //
    size_t const page = PageSize();
    size_t const total = page + wanted;

    void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
    {
        return;
    }

    if (mprotect(base, page, PROT_NONE) != 0)
    {
        munmap(base, total);
        return;
    }

    stack_t next{};
    next.ss_sp = static_cast<uint8_t*>(base) + page;
    next.ss_size = wanted;
    next.ss_flags = 0;

    if (sigaltstack(&next, nullptr) != 0)
    {
        munmap(base, total);
        return;
    }

    m_memory = base;
    m_size = wanted;
    m_installed = true;
}

SignalStack::~SignalStack()
{
    if (!m_installed)
    {
        return;
    }

    // Put back whatever was here first, then unmap. The other order leaves a window in which the
    // kernel would deliver a handler onto memory that is no longer mapped.
    //
    stack_t restore = m_previous;
    if ((restore.ss_flags & SS_DISABLE) != 0 || restore.ss_sp == nullptr)
    {
        restore = stack_t{};
        restore.ss_sp = nullptr;
        restore.ss_size = 0;
        restore.ss_flags = SS_DISABLE;
    }
    sigaltstack(&restore, nullptr);

    munmap(m_memory, PageSize() + m_size);

    m_memory = nullptr;
    m_size = 0;
    m_installed = false;
}

int RegisterOnAltStack(
    int signo,
    void (*handler)(int, siginfo_t*, void*),
    int flags /* = 0 */,
    struct sigaction* previous /* = nullptr */)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = flags | SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    return sigaction(signo, &sa, previous);
}

} // end namespace coop
