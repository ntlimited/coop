#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "coop/context.h"

namespace coop
{
namespace detail
{

// Bump-allocate from the context's segment heap. The heap grows upward from just past the
// Launchable/lambda at the segment's bottom. The stack grows downward from the top. Allocations
// are valid for the lifetime of the context (freed when the segment is returned to the pool).
//
// LIFO de-bumping is supported via BumpFree for temporary allocations that should release space
// before the context exits.
//
inline void* BumpAlloc(Context* ctx, size_t size, size_t align = 16)
{
    uintptr_t top = reinterpret_cast<uintptr_t>(ctx->m_heapTop);
    top = (top + align - 1) & ~(align - 1);
    void* result = reinterpret_cast<void*>(top);
    ctx->m_heapTop = reinterpret_cast<void*>(top + size);

    // Sanity: heap must not collide with the stack. The stack pointer isn't tracked precisely
    // here, but we can check against the segment top as a conservative bound.
    //
    assert(reinterpret_cast<uintptr_t>(ctx->m_heapTop) <
           reinterpret_cast<uintptr_t>(ctx->m_segment.Top()));

    return result;
}

// Restore the heap watermark to a previous position. Strictly LIFO — ptr must be the value
// returned by the corresponding BumpAlloc call (or an earlier one to free multiple allocations).
//
inline void BumpFree(Context* ctx, void* ptr)
{
    assert(reinterpret_cast<uintptr_t>(ptr) >=
           reinterpret_cast<uintptr_t>(ctx->m_segment.Bottom()));
    assert(reinterpret_cast<uintptr_t>(ptr) <=
           reinterpret_cast<uintptr_t>(ctx->m_heapTop));
    ctx->m_heapTop = ptr;
}

} // end namespace coop::detail
} // end namespace coop
