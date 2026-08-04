#pragma once

#include <cstddef>

#include "fixed_buffer.h"

namespace coop
{

namespace io
{

struct Descriptor;

// Zero-copy send (IORING_OP_SEND_ZC, kernel 6.0+). The kernel references the caller's
// pages instead of copying them into skbs, completing in two CQEs: the transfer result,
// then an F_NOTIF once the pages are released. These blocking wrappers wait for BOTH, so
// the buffer is safe to reuse on return — the async single-CQE Handle contract cannot
// express the two-CQE lifetime and is deliberately not offered.
//
// When it pays: >= a few KiB per send (the pin/notify machinery costs more than a copy
// below that — measured knee ~1-4KiB), and never on loopback, where the kernel silently
// falls back to copying. IORING_SEND_ZC_REPORT_USAGE (6.2+) would confirm real zero-copy
// per completion; this host's 6.1 predates it, so treat loopback numbers as copies.
//
// The FixedBuffer overload additionally sources from a registered region (the
// BufferArena) — pin work is pre-paid at registration.
//
int SendZC(Descriptor& desc, const void* buf, size_t size, int flags = 0);
int SendZCKill(Descriptor& desc, const void* buf, size_t size, int flags = 0);
int SendZC(Descriptor& desc, FixedBuffer buf, size_t size, int flags = 0);
int SendZCKill(Descriptor& desc, FixedBuffer buf, size_t size, int flags = 0);

} // end namespace coop::io
} // end namespace coop
