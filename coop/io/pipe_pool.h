#pragma once

namespace coop
{

namespace io
{

// Reusable kernel pipes for splice data paths. A splice hop needs a pipe, and pipe2() per
// transfer is two fds and a syscall on the hot path — the pool caches a handful per uring
// (per thread), created lazily on first use and closed when the uring goes away.
//
// Pipes are returned clean or not at all: a splice loop that aborts between filling and
// draining leaves bytes in the pipe, and a dirty pipe would corrupt its next user — mark
// the lease dirty and it is closed instead of cached.
//
struct PipePool
{
    static constexpr int kMaxCached = 8;

    ~PipePool();

    // Pop a cached pipe or create one. Returns false if pipe2() fails.
    //
    bool Acquire(int pipefd[2]);

    // Return a clean pipe to the cache (or close it when the cache is full).
    //
    void Release(const int pipefd[2]);

    // Close a pipe that may hold undrained bytes.
    //
    void Discard(const int pipefd[2]);

  private:
    int m_pipes[kMaxCached][2];
    int m_count{0};
};

// RAII lease on a pooled pipe. Default release path assumes the pipe was drained; call
// MarkDirty() on any abort between the fill and drain phases of a splice loop.
//
struct PipeLease
{
    explicit PipeLease(PipePool& pool)
    : m_pool(pool)
    , m_ok(pool.Acquire(m_fds))
    {
    }

    ~PipeLease()
    {
        if (!m_ok)
        {
            return;
        }
        if (m_dirty)
        {
            m_pool.Discard(m_fds);
        }
        else
        {
            m_pool.Release(m_fds);
        }
    }

    PipeLease(const PipeLease&) = delete;
    PipeLease& operator=(const PipeLease&) = delete;

    explicit operator bool() const { return m_ok; }
    int* Fds() { return m_fds; }
    void MarkDirty() { m_dirty = true; }

  private:
    PipePool&   m_pool;
    int         m_fds[2];
    bool        m_ok;
    bool        m_dirty{false};
};

} // end namespace coop::io
} // end namespace coop
