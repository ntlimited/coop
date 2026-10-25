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

## Bazel

On Linux x86_64 or aarch64, install [Bazelisk](https://github.com/bazelbuild/bazelisk),
then run:

```bash
bazelisk build //...
bazelisk test //... --runs_per_test=3
```

The pinned Bazel 8 build fetches its dependencies from the public Bazel Central Registry.
The [hermetic LLVM toolchain](https://registry.bazel.build/modules/llvm/0.6.1) supplies
LLVM 21, libc++, Linux headers, and glibc ABI stubs, so no system compiler or development
packages are required. A normal Linux userspace with Bash, coreutils, tar, and network
access to the public archives is needed. Tests require a kernel and syscall policy that
permit `io_uring`, loopback networking, and executable-memory patching for perf mode 2.
No tests are excluded by default. Docker's default seccomp policy may block `io_uring`;
use a container policy that permits its setup, enter, and register syscalls.
On affected Ubuntu 6.8 kernels where provided-buffer registration returns `EINVAL`,
add `--config=ubuntu_pbuf_workaround` to enable coop's existing compatibility path
(CMake: `-DCOOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK=ON`). It stays off by default.

| CMake CI lane | Bazel options |
| --- | --- |
| Debug (x86_64 / aarch64) | `--config=debug` (also the default) |
| Release with LTO (x86_64 / aarch64) | `--config=lto` |
| Release without LTO | `--config=release` |
| Plain `-O2` without LTO | `--config=o2` |

Native architecture selection is automatic. Remote execution endpoints, execution platforms,
credentials, and cache settings belong in your own Bazel rc or the gitignored `user.bazelrc`.
The toolchain carries its inputs to the executor rather than reading the client's `/usr`.
The native platform targets glibc 2.31 or later with Linux 5.7 headers; a sufficiently
recent running kernel is still needed for the tested io_uring features.
`--config=aarch64` selects an ARM target for cross-compilation (it does not emulate tests).

`//:coop` contains the complete runtime, including chan, IO, HTTP, WebSocket, TLS, DNS,
perf, and work. `//:chan` is also a separate library. Every CMake example and benchmark
has a matching root target, such as `//:echo_server` or `//:coop_benchmarks`.
The main suite is `//:coop_tests`; the six assembly/parser/config suites are separate tests.

TLS examples and benchmarks read `cert.pem` and `key.pem` from the working directory.
`//:test_certs` generates development certificates using the fetched OpenSSL tool.
After building, run these binaries from `bazel-bin`, for example
`(cd bazel-bin && ./echo_server)`. The dashboard assets are available there in `static/`.

### Consuming coop from Bazel

Treat a vendored checkout as a module. Bazel globs in a parent package stop at the checkout's
BUILD boundary; compiling its sources again from a parent glob also loses its build contract.
In the consumer's `MODULE.bazel`:

```starlark
bazel_dep(name = "coop", version = "0.1.0")
local_path_override(module_name = "coop", path = "extern/coop")
# Alternatively use git_override(module_name = "coop", remote = <public git URL>,
#                              commit = <pinned commit>).
```

Depend on `@coop//:coop`, or `@coop//:chan` together with `@coop//:coop` when naming the
channel component explicitly. Test consumers can reuse `@coop//:test_helpers`,
`@coop//:test_ws`, and `@coop//:test_introspect` (the latter preserves caller frames).
The helper target supports both `"test_helpers.h"` and `"tests/test_helpers.h"` includes.
`@coop//:test_cases` exports the complete main suite, including its native completion
dispatch object and linker wrapping; link it with a gtest main to embed the suite.
`@coop//:http_static` exports the dashboard files.

The consumer owns its compilation mode and toolchain registration; dependency rc files
are not imported by Bazel. Use C++20 for consumer sources, register the fetched toolchain
with `register_toolchains("@llvm//toolchain:all")` after declaring the same `llvm` dependency,
or supply a compatible Linux C++20 toolchain yourself. Set layout-affecting options in the
consumer's rc so coop and all consumers agree:

```text
build --platforms=@coop//bazel:linux
build --@coop//bazel:default_stack_size=262144
build --@coop//bazel:perf_mode=2
build --@coop//bazel:spdlog_active_level=3
build --@coop//bazel:perf_user_defs=//perf:coop_defs
build --@coop//bazel:perf_user_counters=perf/counters.def
build --@coop//bazel:perf_user_families=perf/families.def
```

`//perf:coop_defs` should be a public `cc_library` with the two `.def` files in
`textual_hdrs` and `strip_include_prefix = "/"` (or equivalent exported include paths).
It must not depend on coop. The label flag makes these declared compiler inputs in coop
and every consumer; the two string flags become quoted include-path definitions.
Alternatively, put the `COOP_PERF_USER_COUNTERS` / `COOP_PERF_USER_FAMILIES` definitions
on that library's `defines` and leave the string flags empty. See
[perf extensions](coop/perf/CLAUDE.md#extension-mechanism-for-coop-consumers) for file syntax.

Defaults are stack size 16384, perf mode 1 (always on), logging level 2 (info), empty
perf paths, and an empty perf-defs library. The standalone release configs select mode 2
(patched counters, zero when off) and logging level 3 (warn). Mode 0 disables counters.
These definitions propagate through `CcInfo`, including to consumer translation units.

`bazel test //...` includes `//bazel/tests:consumer_settings_test`, a fictional consumer
configured through a transition. It exercises nondefault stack/perf/log settings, its own
`.def` headers, dynamic family toggles, and the stack size of runtime-created contexts.

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

## Native HTTP client

For the matching server contracts, see [HTTP server parsing and policy](docs/http_server.md)
and [WebSocket framing and borrowed payloads](docs/websocket.md).

The native client is a streaming HTTP/1.x connection toolkit with borrowed views and
explicit completion/reuse. See [the client contract](docs/http_client.md) for construction,
lifetimes, framing, and caller-owned policy.

The parser regression suite runs without initializing io_uring:

```bash
./build/debug/bin/coop_http_parser_tests
```

`bench_http_parser` isolates parser CPU cost from the network in Release builds.

For explicit TLS trust, identity verification, and SNI setup, see
[the TLS client guide](docs/tls_client.md). The native streaming example is:

```bash
./build/debug/bin/http_fetch example.com 443 / --tls
./build/debug/bin/http_fetch 127.0.0.1 8080 /
./build/debug/bin/coop_tls_config_tests
```

The example uses the existing IPv4/DNS Connect helper and leaves redirects, retries,
and total transaction deadlines to application policy.

## DNS resolution

The [resolver guide](docs/dns_resolver.md) covers explicit configuration, borrowed
nameserver/hosts storage, lookup policy, and the system-configuration convenience API.
`coop_dns_parser_tests` exercises packet and configuration parsing without io_uring.
