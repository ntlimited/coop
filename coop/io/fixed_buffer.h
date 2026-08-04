#pragma once

#include <cstdint>

namespace coop
{

namespace io
{

// A slice of registered (fixed) buffer memory: the pointer must lie inside the region
// registered under `index` (for the BufferArena, index 0 and any address in
// [Base, Base+Size)). Passing one to Read/Write selects the READ_FIXED/WRITE_FIXED
// opcodes — the kernel skips the per-IO get_user_pages/pin work using the registration's
// pre-built page list. The overload set keeps the op NAMES unchanged: fixed-ness is a
// property of the buffer argument, not a different operation.
//
// buf_index and buf_group share one SQE field — a FixedBuffer op can never also be a
// provided-buffer (pbuf group) op. The type system enforces it here: there is no
// FixedBuffer recv overload at all, because no released kernel implements one.
//
struct FixedBuffer
{
    void*    data;
    uint16_t index;
};

} // end namespace coop::io
} // end namespace coop
