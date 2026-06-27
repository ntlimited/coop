#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{

// Per-cooperator free-list pool for detached continuations. A detached continuation is allocated,
// fired, and freed entirely on one cooperator's thread (the single-cooperator invariant), so the
// pool needs no atomics. Blocks are bucketed into size classes with an intrusive free-list overlaid
// on dead blocks; allocations larger than the biggest class fall back to malloc/free. Each bucket
// is capped so a burst of frees does not retain memory unboundedly.
//
class ContinuationPool
{
  public:
    ContinuationPool() = default;
    ContinuationPool(const ContinuationPool&) = delete;
    ContinuationPool& operator=(const ContinuationPool&) = delete;
    ~ContinuationPool();

    // Allocate at least n bytes; Free must be called with the same n the object was sized at
    // (operator delete supplies it), which lands in the same size class.
    //
    void* Allocate(size_t n);
    void  Free(void* p, size_t n);

  private:
    static constexpr size_t   kClasses = 4;
    static constexpr size_t   kSizes[kClasses] = {64, 128, 256, 512};
    static constexpr uint32_t kCap = 256;       // max retained free blocks per class

    struct FreeNode
    {
        FreeNode* next;
    };

    // Smallest class that fits n, or kClasses if n exceeds the largest (unpooled).
    //
    static size_t ClassFor(size_t n)
    {
        for (size_t i = 0; i < kClasses; ++i)
        {
            if (n <= kSizes[i])
            {
                return i;
            }
        }
        return kClasses;
    }

    FreeNode* m_free[kClasses] = {};
    uint32_t  m_count[kClasses] = {};
};

} // end namespace coop
