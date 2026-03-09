# coop/perf/ — Performance Counter Internals

Three compile-time modes controlled by `COOP_PERF_MODE` (CMake cache variable, default 0).
The mode is a PUBLIC compile definition on the `coop` target so consumers and the library
agree on struct layout.

## Modes

**Mode 0 (DISABLED)**: `COOP_PERF_INC` expands to `((void)0)`. `Counters` is an empty struct.
Zero overhead, zero storage.

**Mode 1 (ALWAYS_ON)**: `COOP_PERF_INC` expands to `(counters).Inc(id)` — a direct `uint64_t`
increment. No atomics needed because each cooperator is single-threaded. ~1ns per probe (L1
cache-line hit). `Counters` holds a fixed array indexed by `Counter` enum. Mode 1 ignores
families — all counters are always active.

**Mode 2 (DYNAMIC)**: `COOP_PERF_INC` uses `asm goto` to emit a patchable JMP instruction
that skips the increment by default. Each probe site is registered in the `coop_perf_sites`
ELF section (address + counter ID). At runtime, `Enable()` patches JMPs to NOPs (increments
execute), `Disable()` restores the original JMPs (increments skipped). Modeled after the
Linux kernel `static_key` pattern. Supports per-family selective enable/disable.

## Files

- `counters.h` — `Counter` enum, `Counters` struct, `CounterName()`, `Family` enum,
  `CounterFamily()`, `FamilyName()`, `s_allFamilies[]`
- `probe.h` — `COOP_PERF_INC` macro with three mode implementations
- `patch.h` — `Enable(Family)`/`Disable(Family)`/`SetFamilies()`/`EnabledFamilies()`/
  `Toggle()`/`IsEnabled()`/`ProbeCount()` API; stubs for non-dynamic modes
- `patch.cpp` — Mode 2 patching engine (ELF section scanning, family-aware patching)

## Counter Families

Bitmask-based grouping (`enum class Family : uint64_t`) for selective mode 2 enable/disable.
Family membership is derived from counter ID via `constexpr CounterFamily()` — no ELF section
changes needed. Patching only touches sites whose enabled state actually changes.

| Family      | Bit  | Counters                                                              |
|-------------|------|-----------------------------------------------------------------------|
| `Scheduler` | 0x01 | SchedulerLoop, ContextResume/Yield/Block/Spawn/Exit                   |
| `IO`        | 0x02 | IoSubmit, IoComplete, PollCycle/Submit/Cqe                            |
| `Epoch`     | 0x04 | EpochAdvance/Pin/Unpin, DrainCycles/Reclaimed                         |

API: `Enable(Family::Scheduler | Family::IO)`, `Disable(Family::IO)`,
`SetFamilies(Family::Scheduler)`, `EnabledFamilies()`.

## Counter Table

### Scheduler Family

| Counter         | Probe location                    | Notes                                    |
|-----------------|-----------------------------------|------------------------------------------|
| `SchedulerLoop` | `cooperator.cpp` inner resume loop| Per-context-resume iteration              |
| `ContextResume` | `Cooperator::Resume()`            | Cooperator -> context switch              |
| `ContextYield`  | `HandleCooperatorResumption`      | Voluntary yield (context -> cooperator)   |
| `ContextBlock`  | `HandleCooperatorResumption`      | Transition to BLOCKED state               |
| `ContextSpawn`  | `Cooperator::EnterContext()`      | New context creation                      |
| `ContextExit`   | `HandleCooperatorResumption`      | Context destruction (stack freed)         |

### IO Family

| Counter         | Probe location                    | Notes                                    |
|-----------------|-----------------------------------|------------------------------------------|
| `IoSubmit`      | `Handle::Submit()`                | io_uring SQE submission                   |
| `IoComplete`    | `Handle::Complete()`              | CQE completion                            |
| `PollCycle`     | `Uring::Poll()` top               | Poll invocations                          |
| `PollSubmit`    | `Uring::Poll()` submit branch     | Polls that actually submitted SQEs        |
| `PollCqe`       | `Uring::Poll()` CQE loop          | Individual CQEs processed                 |
### Epoch Family

| Counter              | Probe location                        | Notes                                    |
|----------------------|---------------------------------------|------------------------------------------|
| `EpochAdvance`       | Manager::Advance                      | Epoch ticks                              |
| `EpochPin`           | Manager::Pin (application slot)       | Transaction-level pins                   |
| `EpochUnpin`         | Manager::Unpin (application slot)     | Transaction-level unpins                 |
| `DrainCycles`        | DrainTable call site                  | Reclamation attempts                     |
| `DrainReclaimed`     | DrainTable return value accumulation  | Nodes actually freed                     |

## Dynamic Patching Engine (`patch.cpp`)

**Probe discovery**: linker-generated `__start_coop_perf_sites` / `__stop_coop_perf_sites`
(weak symbols) bound the ELF section. Each 16-byte entry contains the probe site address and
counter ID. `InitSites()` scans the section on first call, detects JEB (2-byte `0xEB`) vs
JMP (5-byte `0xE9`) encodings, and saves original bytes.

**Family-aware patching**: `s_enabledFamilies` (replaces the old `s_enabled` bool) tracks
which families are active. `Enable(families)` OR's into the mask and patches newly-enabled
sites. `Disable(families)` AND-NOT's the mask and restores newly-disabled sites. Only sites
whose enabled state actually changes are patched — toggling one family doesn't re-patch others.

**Thread safety**: patching is safe under cooperative scheduling — between context switches
only the cooperator thread executes, so no probe site is being executed concurrently during
patching. On x86-64, the 2-byte NOP/JMP write is atomic (naturally aligned), and the icache
is coherent with the dcache (syncs on next taken branch).

## Status API

`/api/perf` returns mode, enabled state, family breakdown, and all counter values:
```json
{
    "mode": 2,
    "enabledFamilies": 7,
    "families": {
        "scheduler": {"bit": 1, "enabled": true},
        "io": {"bit": 2, "enabled": true},
        ...
    },
    "counters": { ... }
}
```

`/api/perf/enable?families=scheduler,epoch` — selective enable.
`/api/perf/disable?families=io` — selective disable.

## Testing Considerations

- Mode 1 tests: the test context enters via `EnterContext` (Submit path), which counts
  `ContextSpawn` but not `ContextResume`. Must yield to generate resume/scheduler activity.
- IO counters (`IoSubmit`/`IoComplete`): the fast-path optimization in Recv/Send means
  socketpair IO with pre-staged data succeeds via direct syscall without going through
  `Handle::Submit`. Tests that need IO counter activity should use operations that cannot
  fast-path (e.g., Accept, or recv on an empty socket).
- Mode 2 tests: `ProbeCount()` triggers `InitSites()` and should return > 0 when the binary
  has instrumented code. Counters are zero until `Enable()` is called.
- Family tests: mode-independent tests verify `CounterFamily()` mapping and `FamilyName()`.
  Mode 2 tests verify selective enable/disable (`FamilySelectiveEnable`, `SetFamilies`).

## CPU Sampler (`sampler.h`, `sampler.cpp`)

SIGPROF-based CPU profiler, independent of perf counters. Uses `ITIMER_PROF` to fire at a
configurable Hz, captures RIP + current context + cooperator from the signal handler into a
lock-free ring buffer (8192 entries). Per-context `m_statistics.samples` is also incremented
in the handler (safe: same thread). Each `Sample` includes the `Cooperator*` that was active,
enabling per-cooperator grouping in multi-cooperator processes.

**API**: `StartSampling(hz)`, `StopSampling()`, `IsSampling()`, `SamplingHz()`,
`ReadSamples(out, max)`, `ResetSamples()`, `TotalSamples()`, `SampleCapacity()`.

`ResetSamples()` clears the ring buffer head and total count — call before `StartSampling()`
to get a clean sample window. The `/api/sampler/start` endpoint does this automatically.

**Dashboard integration**: The status server exposes `/api/sampler/*` endpoints plus
`/api/sampler/symbolize` (POST, comma-separated hex PCs) which uses `dladdr()` +
`__cxa_demangle()` for server-side symbol resolution. The frontend runs periodic 1-second
burst sampling cycles and renders a 2-level flame graph (contexts → functions).

Executables that want useful symbol resolution need `-rdynamic` link flag (exports symbols to
the dynamic symbol table for `dladdr()`).

## Adding New Probes (within coop)

1. Add the counter to the `Counter` enum in `counters.h` (before the user-defined block)
2. Add the name string to `CounterName()` in the same position
3. Add a case in `CounterFamily()` mapping the counter to its family
4. Insert `COOP_PERF_INC(counters_ref, perf::Counter::NewCounter)` at the probe site
5. The counters reference is typically `m_perf` (cooperator member) accessed via
   `GetPerfCounters()`, or `coop::GetCooperator()->GetPerfCounters()` for code
   that doesn't have a direct cooperator pointer

## Extension Mechanism (for coop consumers)

Consumers that embed coop as a submodule can define their own counters and families via
X-macro `.def` files. This gives extension counters full parity with coop-native ones:
compile-time enum IDs (mode 2 compatible), per-family selective enable/disable, automatic
integration with the status API and observability endpoints.

### Setup

Define two compile definitions pointing to `.def` file paths:

```cmake
# In the consumer's CMakeLists.txt:
target_compile_definitions(coop PUBLIC
    COOP_PERF_USER_FAMILIES="${CMAKE_CURRENT_SOURCE_DIR}/perf/my_families.def"
    COOP_PERF_USER_COUNTERS="${CMAKE_CURRENT_SOURCE_DIR}/perf/my_counters.def"
)
```

Both defines are optional — you can add counters to existing coop families without defining
new families, or define new families without adding counters (unusual but valid).

### Families file format

Each line uses the `COOP_PERF_FAMILY(name, bit, display)` X-macro:
- `name` — C++ identifier, becomes `Family::name`
- `bit` — bit index in the `uint64_t` bitmask. Coop reserves bits 0-15; use 16+
- `display` — string literal for `FamilyName()` and the status API

```cpp
// my_families.def
COOP_PERF_FAMILY(Scan, 16, "scan")
COOP_PERF_FAMILY(DML,  17, "dml")
COOP_PERF_FAMILY(Txn,  18, "txn")
```

### Counters file format

Each line uses the `COOP_PERF_COUNTER(name, family, display)` X-macro:
- `name` — C++ identifier, becomes `Counter::name`
- `family` — must match a `Family::` enum value (coop-native or user-defined)
- `display` — string literal for `CounterName()` and the status API

```cpp
// my_counters.def
COOP_PERF_COUNTER(TuplesScanned,       Scan, "tuples_scanned")
COOP_PERF_COUNTER(TuplesReturned,      Scan, "tuples_returned")
COOP_PERF_COUNTER(TuplesInserted,      DML,  "tuples_inserted")
COOP_PERF_COUNTER(TuplesDeleted,       DML,  "tuples_deleted")
COOP_PERF_COUNTER(TxnBegin,            Txn,  "txn_begin")
COOP_PERF_COUNTER(TxnCommit,           Txn,  "txn_commit")
```

### Usage in consumer code

Extension counters work identically to coop-native counters:

```cpp
#include "coop/perf/probe.h"

// In hot path code:
COOP_PERF_INC(coop::GetCooperator()->GetPerfCounters(), coop::perf::Counter::TuplesScanned);

// Family-level control:
coop::perf::Enable(coop::perf::Family::Scan | coop::perf::Family::Txn);
coop::perf::Disable(coop::perf::Family::DML);
```

### What it expands to

`counters.h` includes each `.def` file at six points, redefining the X-macro each time to
extract the right field:

1. `Counter` enum — `COOP_PERF_COUNTER(name, ...) → name,`
2. `CounterName()` array — `COOP_PERF_COUNTER(..., display) → display,`
3. `CounterFamily()` switch — `COOP_PERF_COUNTER(name, family, ...) → case Counter::name: return Family::family;`
4. `Family` enum — `COOP_PERF_FAMILY(name, bit, ...) → name = 1ULL << bit,`
5. `FamilyName()` switch — `COOP_PERF_FAMILY(name, ..., display) → case Family::name: return display;`
6. `s_allFamilies[]` array — `COOP_PERF_FAMILY(name, ...) → Family::name,`

The `Counters::values[]` array, mode 2 ELF section probes, patching engine, and status API
all adapt automatically — no changes needed outside the `.def` files.

### Design constraints

- **One extension layer**: only one set of `.def` files can be active. This matches the
  typical deployment model (one application embedding coop as a submodule).
- **Rebuild required**: changing `.def` files requires rebuilding coop. Natural since the
  consumer controls the build.
- **Mode 2 compatible**: extension counter IDs are enum values (compile-time constants), so
  `asm goto` immediate operands work. All three modes have full parity.
