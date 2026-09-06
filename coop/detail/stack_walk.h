#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::detail
{

// A frame pointer is only a candidate address. Check the entire record before loading either
// word, including at a guard-page boundary. Bounds must describe known live, readable memory;
// they cannot make a concurrently destroyed stack safe to inspect.
//
struct StackBounds
{
    uintptr_t lo;
    uintptr_t hi;

    bool Contains(uintptr_t address, size_t bytes) const
    {
        return address >= lo && address <= hi && bytes <= hi - address;
    }

    bool ContainsWords(uintptr_t address, size_t count) const
    {
        return (address & (alignof(uintptr_t) - 1)) == 0 &&
               Contains(address, count * sizeof(uintptr_t));
    }
};

inline int WalkFrameChain(uintptr_t fp, StackBounds bounds, uintptr_t* frames, int maxFrames)
{
    int depth = 0;
    while (depth < maxFrames && bounds.ContainsWords(fp, 2))
    {
        auto* frame = reinterpret_cast<const uintptr_t*>(fp);
        uintptr_t ret = frame[1];
        if (!ret) break;
        frames[depth++] = ret;

        uintptr_t next = frame[0];
        if (next <= fp) break;   // also terminates cyclic and self-referential chains
        fp = next;
    }
    return depth;
}

// Recover the switch call site and its frame pointer from context_switch.S's saved register
// record. Validate the complete save area, not just the first word at savedSp.
//
inline bool ReadSavedStack(uintptr_t savedSp, StackBounds bounds, uintptr_t& pc, uintptr_t& fp)
{
#if defined(__x86_64__)
    constexpr size_t words = 7;  // r15, r14, r13, r12, rbx, rbp, return address
    constexpr size_t fpSlot = 5;
    constexpr size_t pcSlot = 6;
#elif defined(__aarch64__)
    constexpr size_t words = 12; // x29, x30, then x19-x28
    constexpr size_t fpSlot = 0;
    constexpr size_t pcSlot = 1;
#else
#error "Unsupported architecture for saved stack capture"
#endif
    if (!savedSp || !bounds.ContainsWords(savedSp, words)) return false;
    auto* saved = reinterpret_cast<const uintptr_t*>(savedSp);
    fp = saved[fpSlot];
    pc = saved[pcSlot];
    return true;
}

// A signal may interrupt the scheduler or a stack switch. Scheduled() alone does not prove we
// are on that context's stack: require the interrupted SP to belong to the supplied segment.
// Native stacks without pre-established bounds deliberately yield only the interrupted PC.
//
inline int WalkInterruptedStack(uintptr_t pc, uintptr_t fp, uintptr_t sp, StackBounds bounds,
                                uintptr_t* frames, int maxFrames)
{
    if (maxFrames <= 0) return 0;
    frames[0] = pc;
    if (!bounds.Contains(sp, 1)) return 1;
    bounds.lo = sp;
    return 1 + WalkFrameChain(fp, bounds, frames + 1, maxFrames - 1);
}

} // namespace coop::detail
