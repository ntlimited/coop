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
| `Epoch`     | 0x20 | EpochAdvance/Pin/Unpin, DrainCycles/Reclaimed                         |

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

## Adding New Probes

1. Add the counter to the `Counter` enum in `counters.h` (before `COUNT`)
2. Add the name string to `CounterName()` in the same position
3. Add a case in `CounterFamily()` mapping the counter to its family
4. Insert `COOP_PERF_INC(counters_ref, perf::Counter::NewCounter)` at the probe site
5. The counters reference is typically `m_perf` (cooperator member) accessed via
   `GetPerfCounters()`, or `coop::GetCooperator()->GetPerfCounters()` for code
   that doesn't have a direct cooperator pointer (e.g.)
