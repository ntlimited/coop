#pragma once

#include "coop/time/interval.h"

// Visitor projections for operation argument lists.
//
// Each operation defines an ARGS macro like:
//   #define RECV_ARGS(F) F(void*, buf, ) F(size_t, size, ) F(int, flags, = 0)
//
// The leading-comma convention means the preceding parameter (Handle&, Descriptor&, or sqe+fd)
// absorbs the first comma naturally. Zero-arg operations (Close) define an empty ARGS body.
//
#define COOP_IO_ARG_DECL(type, name, default) , type name default
#define COOP_IO_ARG_DEF(type, name, default) , type name
#define COOP_IO_ARG_FWD(type, name, default) , name

// Generate all 4 declarations for a standard IO operation. Timeout variants strip defaults from
// preceding args so timeout is always explicitly provided.
//
#define COOP_IO_DECLARATIONS(name, ARGS)                                                \
    bool name(Handle& handle ARGS(COOP_IO_ARG_DECL));                                  \
    bool name(Handle& handle ARGS(COOP_IO_ARG_DEF), time::Interval timeout);           \
    int name(Descriptor& desc ARGS(COOP_IO_ARG_DECL));                                 \
    int name(Descriptor& desc ARGS(COOP_IO_ARG_DEF), time::Interval timeout);

// Individual implementation macros, exposed for operations that only need a subset.
//
#define COOP_IO_ASYNC_IMPL(name, prep_fn, ARGS)                                         \
    bool name(Handle& handle ARGS(COOP_IO_ARG_DEF))                                     \
    {                                                                                    \
        auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);                           \
        if (!sqe)                                                                        \
        {                                                                                \
            return false;                                                                \
        }                                                                                \
        prep_fn(sqe, handle.m_descriptor->m_fd ARGS(COOP_IO_ARG_FWD));                  \
        handle.Submit(sqe);                                                              \
        return true;                                                                     \
    }

#define COOP_IO_ASYNC_TIMEOUT_IMPL(name, prep_fn, ARGS)                                 \
    bool name(Handle& handle ARGS(COOP_IO_ARG_DEF), time::Interval timeout)             \
    {                                                                                    \
        auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);                           \
        if (!sqe)                                                                        \
        {                                                                                \
            return false;                                                                \
        }                                                                                \
        prep_fn(sqe, handle.m_descriptor->m_fd ARGS(COOP_IO_ARG_FWD));                  \
        handle.SubmitWithTimeout(sqe, timeout);                                          \
        return true;                                                                     \
    }

#define COOP_IO_BLOCKING_IMPL(name, ARGS)                                                \
    int name(Descriptor& desc ARGS(COOP_IO_ARG_DEF))                                    \
    {                                                                                    \
        Coordinator coord;                                                               \
        Handle handle(Self(), desc, &coord);                                             \
        if (!name(handle ARGS(COOP_IO_ARG_FWD)))                                        \
        {                                                                                \
            return -EAGAIN;                                                              \
        }                                                                                \
        return handle;                                                                   \
    }

#define COOP_IO_BLOCKING_TIMEOUT_IMPL(name, ARGS)                                       \
    int name(Descriptor& desc ARGS(COOP_IO_ARG_DEF), time::Interval timeout)            \
    {                                                                                    \
        Coordinator coord;                                                               \
        Handle handle(Self(), desc, &coord);                                             \
        if (!name(handle ARGS(COOP_IO_ARG_FWD), timeout))                               \
        {                                                                                \
            return -EAGAIN;                                                              \
        }                                                                                \
        int result = handle;                                                             \
        if (handle.TimedOut())                                                           \
        {                                                                                \
            return -ETIMEDOUT;                                                           \
        }                                                                                \
        return result;                                                                   \
    }

// Generate all 4 implementations for a standard IO operation.
//
#define COOP_IO_IMPLEMENTATIONS(name, prep_fn, ARGS)                                     \
    COOP_IO_ASYNC_IMPL(name, prep_fn, ARGS)                                              \
    COOP_IO_ASYNC_TIMEOUT_IMPL(name, prep_fn, ARGS)                                      \
    COOP_IO_BLOCKING_IMPL(name, ARGS)                                                    \
    COOP_IO_BLOCKING_TIMEOUT_IMPL(name, ARGS)

// -------------------------------------------------------------------------------------
// Uring-level operations (AT_FDCWD pattern)
//
// For operations like open, unlink, mkdir that don't operate on an existing descriptor.
// The async flavor takes Handle& (same as descriptor ops), but the prep call passes
// AT_FDCWD instead of a descriptor fd. The blocking flavor takes no Descriptor& — it
// obtains the ring via GetUring() and constructs the Handle directly.
// -------------------------------------------------------------------------------------

// Helper: eats the leading comma from an ARGS(PROJ) expansion so it can appear as the
// first (or only) parameter list. Works by matching the empty token before the first comma
// as `first_`, then forwarding the rest.
//
#define COOP_IO_NO_LEAD_INNER(first_, ...) __VA_ARGS__
#define COOP_IO_NO_LEAD(...) COOP_IO_NO_LEAD_INNER(__VA_ARGS__)

#define COOP_IO_URING_DECLARATIONS(name, ARGS)                                           \
    bool name(Handle& handle ARGS(COOP_IO_ARG_DECL));                                   \
    bool name(Handle& handle ARGS(COOP_IO_ARG_DEF), time::Interval timeout);            \
    int name(COOP_IO_NO_LEAD(ARGS(COOP_IO_ARG_DECL)));                                  \
    int name(COOP_IO_NO_LEAD(ARGS(COOP_IO_ARG_DEF)), time::Interval timeout);

#define COOP_IO_URING_ASYNC_IMPL(name, prep_fn, ARGS)                                    \
    bool name(Handle& handle ARGS(COOP_IO_ARG_DEF))                                      \
    {                                                                                     \
        auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);                            \
        if (!sqe)                                                                         \
        {                                                                                 \
            return false;                                                                 \
        }                                                                                 \
        prep_fn(sqe, AT_FDCWD ARGS(COOP_IO_ARG_FWD));                                   \
        handle.Submit(sqe);                                                               \
        return true;                                                                      \
    }

#define COOP_IO_URING_ASYNC_TIMEOUT_IMPL(name, prep_fn, ARGS)                            \
    bool name(Handle& handle ARGS(COOP_IO_ARG_DEF), time::Interval timeout)              \
    {                                                                                     \
        auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);                            \
        if (!sqe)                                                                         \
        {                                                                                 \
            return false;                                                                 \
        }                                                                                 \
        prep_fn(sqe, AT_FDCWD ARGS(COOP_IO_ARG_FWD));                                   \
        handle.SubmitWithTimeout(sqe, timeout);                                           \
        return true;                                                                      \
    }

#define COOP_IO_URING_BLOCKING_IMPL(name, ARGS)                                           \
    int name(COOP_IO_NO_LEAD(ARGS(COOP_IO_ARG_DEF)))                                     \
    {                                                                                     \
        auto* ring = GetUring();                                                          \
        Coordinator coord;                                                                \
        Handle handle(Self(), ring, &coord);                                              \
        if (!name(handle ARGS(COOP_IO_ARG_FWD)))                                         \
        {                                                                                 \
            return -EAGAIN;                                                               \
        }                                                                                 \
        return handle;                                                                    \
    }

#define COOP_IO_URING_BLOCKING_TIMEOUT_IMPL(name, ARGS)                                   \
    int name(COOP_IO_NO_LEAD(ARGS(COOP_IO_ARG_DEF)), time::Interval timeout)             \
    {                                                                                     \
        auto* ring = GetUring();                                                          \
        Coordinator coord;                                                                \
        Handle handle(Self(), ring, &coord);                                              \
        if (!name(handle ARGS(COOP_IO_ARG_FWD), timeout))                                \
        {                                                                                 \
            return -EAGAIN;                                                               \
        }                                                                                 \
        int result = handle;                                                              \
        if (handle.TimedOut())                                                            \
        {                                                                                 \
            return -ETIMEDOUT;                                                            \
        }                                                                                 \
        return result;                                                                    \
    }

// Generate all 4 implementations for a uring-level IO operation.
//
#define COOP_IO_URING_IMPLEMENTATIONS(name, prep_fn, ARGS)                                \
    COOP_IO_URING_ASYNC_IMPL(name, prep_fn, ARGS)                                        \
    COOP_IO_URING_ASYNC_TIMEOUT_IMPL(name, prep_fn, ARGS)                                \
    COOP_IO_URING_BLOCKING_IMPL(name, ARGS)                                              \
    COOP_IO_URING_BLOCKING_TIMEOUT_IMPL(name, ARGS)
