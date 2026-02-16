#include <cassert>
#include <cstdlib>

#ifndef NDEBUG
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "stack_pool.h"
#include "context.h"

namespace coop
{

StackPool::~StackPool()
{
    Drain();
}

size_t StackPool::RoundUpStackSize(size_t stackSize)
{
    if (stackSize <= MIN_STACK_SIZE) return MIN_STACK_SIZE;
    if (stackSize > MAX_POOLED_STACK) return stackSize;

    // Round up to next power of 2
    //
    stackSize--;
    stackSize |= stackSize >> 1;
    stackSize |= stackSize >> 2;
    stackSize |= stackSize >> 4;
    stackSize |= stackSize >> 8;
    stackSize |= stackSize >> 16;
    stackSize |= stackSize >> 32;
    stackSize++;
    return stackSize;
}

void* StackPool::Allocate(size_t stackSize)
{
    if (stackSize > MAX_POOLED_STACK)
    {
        m_misses++;
        return RawAllocate(stackSize);
    }

    int idx = BucketIndex(stackSize);
    auto& bucket = m_buckets[idx];

    if (bucket.head)
    {
        auto* node = bucket.head;
        bucket.head = node->next;
        bucket.count--;
        m_cachedBytes -= sizeof(Context) + stackSize;
        m_hits++;
        return node;
    }

    m_misses++;
    return RawAllocate(stackSize);
}

void StackPool::Free(void* ptr, size_t stackSize)
{
    if (stackSize > MAX_POOLED_STACK)
    {
        RawFree(ptr, stackSize);
        return;
    }

    int idx = BucketIndex(stackSize);
    auto& bucket = m_buckets[idx];

    if (bucket.count >= MAX_PER_BUCKET)
    {
        // Evict the oldest (head) to make room for the new entry
        //
        auto* evict = bucket.head;
        bucket.head = evict->next;
        bucket.count--;
        m_cachedBytes -= sizeof(Context) + stackSize;
        RawFree(evict, stackSize);
    }

    // Overlay a FreeNode at the start of the dead allocation
    //
    auto* node = static_cast<FreeNode*>(ptr);
    node->next = bucket.head;
    bucket.head = node;
    bucket.count++;
    m_cachedBytes += sizeof(Context) + stackSize;
}

void StackPool::Drain()
{
    for (int i = 0; i < NUM_BUCKETS; i++)
    {
        size_t size = BucketSize(i);
        auto* node = m_buckets[i].head;
        while (node)
        {
            auto* next = node->next;
            RawFree(node, size);
            node = next;
        }
        m_buckets[i].head = nullptr;
        m_buckets[i].count = 0;
    }
    m_cachedBytes = 0;
}

StackPool::Stats StackPool::GetStats() const
{
    size_t cached = 0;
    for (int i = 0; i < NUM_BUCKETS; i++)
    {
        cached += m_buckets[i].count;
    }
    return Stats{cached, m_cachedBytes, m_hits, m_misses};
}

int StackPool::BucketIndex(size_t stackSize)
{
    // stackSize must be a power of 2 in [MIN_STACK_SIZE, MAX_POOLED_STACK]
    //
    int bits = __builtin_ctzl(stackSize);
    int idx = bits - 12;  // 12 = log2(4096)
    assert(idx >= 0 && idx < NUM_BUCKETS);
    return idx;
}

size_t StackPool::BucketSize(int index)
{
    return MIN_STACK_SIZE << index;
}

// ---------------------------------------------------------------------------
// Raw allocation — extracted from the original AllocateContext/FreeContext
// ---------------------------------------------------------------------------

#ifndef NDEBUG

void* StackPool::RawAllocate(size_t stackSize)
{
    assert((stackSize & 127) == 0);

    static const size_t kPageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));

    size_t usable = sizeof(Context) + stackSize;
    size_t usableAligned = (usable + kPageSize - 1) & ~(kPageSize - 1);
    size_t total = kPageSize + usableAligned + kPageSize;

    void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return nullptr;

    mprotect(base, kPageSize, PROT_NONE);
    mprotect(static_cast<uint8_t*>(base) + kPageSize + usableAligned, kPageSize, PROT_NONE);

    return static_cast<uint8_t*>(base) + kPageSize;
}

void StackPool::RawFree(void* ptr, size_t stackSize)
{
    static const size_t kPageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));

    size_t usable = sizeof(Context) + stackSize;
    size_t usableAligned = (usable + kPageSize - 1) & ~(kPageSize - 1);
    size_t total = kPageSize + usableAligned + kPageSize;

    void* base = static_cast<uint8_t*>(ptr) - kPageSize;
    munmap(base, total);
}

#else

void* StackPool::RawAllocate(size_t stackSize)
{
    assert((stackSize & 127) == 0);
    return malloc(sizeof(Context) + stackSize);
}

void StackPool::RawFree(void* ptr, size_t)
{
    free(ptr);
}

#endif

} // end namespace coop
