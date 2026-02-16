#pragma once

#include <cstddef>

namespace coop
{

// A size-class allocator with per-bucket free lists for context stack allocations. Owned by
// Cooperator (single-threaded, no locking required). Caches freed allocations to eliminate
// mmap/munmap (debug) or malloc/free (release) syscalls on the hot path.
//
// Size classes are powers of 2 from 4KB to 128KB. Requests round up to the next bucket.
// Requests larger than 128KB bypass the pool entirely.
//
struct StackPool
{
    ~StackPool();

    void* Allocate(size_t stackSize);
    void Free(void* ptr, size_t stackSize);
    void Drain();

    // Round a requested stack size up to the nearest pool bucket boundary (power of 2,
    // minimum 4KB). Sizes above MAX_POOLED_STACK are returned unchanged.
    //
    static size_t RoundUpStackSize(size_t stackSize);

    struct Stats
    {
        size_t cached;
        size_t totalBytes;
        size_t hits;
        size_t misses;
    };

    Stats GetStats() const;

  private:
    struct FreeNode { FreeNode* next; };

    struct Bucket
    {
        FreeNode* head = nullptr;
        int count = 0;
    };

    static constexpr int NUM_BUCKETS = 6;              // 4KB .. 128KB
    static constexpr int MAX_PER_BUCKET = 32;
    static constexpr size_t MIN_STACK_SIZE = 4096;
    static constexpr size_t MAX_POOLED_STACK = 131072;

    Bucket m_buckets[NUM_BUCKETS];

    size_t m_hits = 0;
    size_t m_misses = 0;
    size_t m_cachedBytes = 0;

    static int BucketIndex(size_t stackSize);
    static size_t BucketSize(int index);

    // Underlying alloc/free — mmap+guard pages (debug) or malloc (release)
    //
    static void* RawAllocate(size_t stackSize);
    static void RawFree(void* ptr, size_t stackSize);
};

} // end namespace coop
