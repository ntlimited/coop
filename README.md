# coop

`coop` is a C++20 cooperative multitasking runtime with an `io_uring`-first IO layer.
It is designed for low-latency services that want synchronous-looking code without OS-thread-per-task overhead.

## Quick Start

Prerequisites:
- CMake 3.16+
- C++20 compiler
- `liburing`
- OpenSSL 3.x

Build and run tests (debug):

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
./build/debug/bin/coop_tests
```

Build and run tests (release):

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
./build/release/bin/coop_tests
```

Run a focused test set:

```bash
./build/debug/bin/coop_tests --gtest_filter='IoTest.*:ShutdownTest.*'
```

## Repository Map

- `coop/`: runtime implementation (scheduler, context lifecycle, IO, HTTP, perf)
- `tests/`: gtest coverage
- `benchmarks/`: benchmark suite
- `examples/`: sample applications
- `DESIGN_IDIOMS.md`: design and API principles
- `CLAUDE.md`: detailed architecture guide
- `AGENTS.md`: fast Codex-focused operating guide

## Core Concepts

- `Cooperator`: per-thread scheduler for `Context` instances.
- `Context`: stackful execution unit with parent/child lifecycle.
- `Coordinator`: synchronization primitive used for blocking/unblocking contexts.
- `io::Uring`: `io_uring` wrapper used by cooperative IO operations.
- `io::Handle`: operation handle used by async IO entry points.

## Reading Order

For non-trivial changes, this order keeps ramp-up fast:

1. `AGENTS.md`
2. `CLAUDE.md`
3. `coop/CLAUDE.md`
4. component docs (`coop/io/CLAUDE.md`, `coop/http/CLAUDE.md`, `coop/perf/CLAUDE.md`, `tests/CLAUDE.md`)
5. `DESIGN_IDIOMS.md`

## Troubleshooting

### `uring init failed ...`

`coop` now fails fast when `io_uring` initialization fails. Common causes:
- kernel too old for requested flags
- sandbox/seccomp restrictions
- missing privileges for specific modes (for example SQPOLL)

When this happens, run on a host or container with `io_uring` enabled and less restrictive syscall policy.

### Test selection while iterating

Use targeted `--gtest_filter` runs for fast loops, then run the full suite before merging.

## Standalone vs Subdirectory

When `coop` is used as a standalone project (`cmake -S . -B ...`), tests, benchmarks, and examples are built.
When included as a subdirectory, only the `coop` library target is built by default.
