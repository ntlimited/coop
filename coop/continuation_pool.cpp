#include "continuation_pool.h"

#include <cstdlib>

namespace coop
{

constexpr size_t ContinuationPool::kSizes[];

void* ContinuationPool::Allocate(size_t n)
{
    const size_t c = ClassFor(n);
    if (c == kClasses)
    {
        return std::malloc(n);                  // larger than any class: unpooled
    }

    if (FreeNode* node = m_free[c])
    {
        m_free[c] = node->next;
        --m_count[c];
        return node;
    }

    return std::malloc(kSizes[c]);
}

void ContinuationPool::Free(void* p, size_t n)
{
    const size_t c = ClassFor(n);
    if (c == kClasses || m_count[c] >= kCap)
    {
        std::free(p);                           // unpooled, or bucket already full
        return;
    }

    FreeNode* node = static_cast<FreeNode*>(p);
    node->next = m_free[c];
    m_free[c] = node;
    ++m_count[c];
}

ContinuationPool::~ContinuationPool()
{
    for (size_t c = 0; c < kClasses; ++c)
    {
        for (FreeNode* node = m_free[c]; node;)
        {
            FreeNode* next = node->next;
            std::free(node);
            node = next;
        }
    }
}

} // end namespace coop
