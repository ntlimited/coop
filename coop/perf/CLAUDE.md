# coop/perf/ — Performance Counter Internals

Three compile-time modes controlled by `COOP_PERF_MODE` (CMake cache variable, default 0).
The mode is a PUBLIC compile definition on the `coop` target so consumers and the library
agree on struct layout.

## Modes

**Mode 0 (DISABLED)**: `COOP_PERF_INC` expands to `((void)0)`. `Counters` is an empty struct.
Zero overhead, zero storage.

**Mode 1 (ALWAYS_ON)**: `COOP_PERF_INC` expands to `(counters).Inc(id)` — a direct `uint64_t`
increment. No atomics needed because each cooperator is single-threaded. ~1ns per probe (L1
cache-line hit). `Counters` holds a fixed array indexed by `Counter` enum.

**Mode 2 (DYNAMIC)**: `COOP_PERF_INC` uses `asm goto` to emit a patchable JMP instruction
that skips the increment by default. Each probe site is registered in the `coop_perf_sites`
ELF section (address + counter ID). At runtime, `Enable()` patches JMPs to NOPs (increments
execute), `Disable()` restores the original JMPs (increments skipped). Modeled after the
Linux kernel `static_key` pattern.

## Files

- `counters.h` — `Counter` enum, `Counters` struct (array + `Inc`/`Get`/`Reset`), empty
  struct for mode 0, `CounterName()` for human-readable labels
- `probe.h` — `COOP_PERF_INC` macro with three mode implementations
- `patch.h` — `Enable()`/`Disable()`/`Toggle()`/`IsEnabled()`/`ProbeCount()` API; stubs for
  non-dynamic modes
- `patch.cpp` — Mode 2 patching engine (ELF section scanning, `mprotect`-based code patching)

## Counter Enum (`counters.h`)

| Counter         | Probe location                    | Notes                                    |
|-----------------|-----------------------------------|------------------------------------------|
| `SchedulerLoop` | `cooperator.cpp` inner resume loop| Per-context-resume iteration              |
| `ContextResume` | `Cooperator::Resume()`            | Cooperator -> context switch              |
| `ContextYield`  | `HandleCooperatorResumption`      | Voluntary yield (context -> cooperator)   |
| `ContextBlock`  | `HandleCooperatorResumption`      | Transition to BLOCKED state               |
| `ContextSpawn`  | `Cooperator::EnterContext()`      | New context creation                      |
| `ContextExit`   | `HandleCooperatorResumption`      | Context destruction (stack freed)         |
| `IoSubmit`      | `Handle::Submit()`                | io_uring SQE submission                   |
| `IoComplete`    | `Handle::Complete()`              | CQE completion                            |
| `PollCycle`     | `Uring::Poll()` top               | Poll invocations                          |
| `PollSubmit`    | `Uring::Poll()` submit branch     | Polls that actually submitted SQEs        |
| `PollCqe`       | `Uring::Poll()` CQE loop          | Individual CQEs processed                 |

## Dynamic Patching Engine (`patch.cpp`)

**Probe discovery**: linker-generated `__start_coop_perf_sites` / `__stop_coop_perf_sites`
(weak symbols) bound the ELF section. Each 16-byte entry contains the probe site address and
counter ID. `InitSites()` scans the section on first call, detects JEB (2-byte `0xEB`) vs
JMP (5-byte `0xE9`) encodings, and saves original bytes.

**Patching**: `PatchBytes()` uses `mprotect` to make the containing page(s) `RWX`, writes
the new bytes, then restores `RX`. Enable patches JMPs to canonical NOPs (`0x66 0x90` for
2-byte, `0x0F 0x1F 0x44 0x00 0x00` for 5-byte). Disable restores saved original bytes.

**Thread safety**: patching is safe under cooperative scheduling — between context switches
only the cooperator thread executes, so no probe site is being executed concurrently during
patching. On x86-64, the 2-byte NOP/JMP write is atomic (naturally aligned), and the icache
is coherent with the dcache (syncs on next taken branch).

## Testing Considerations

- Mode 1 tests: the test context enters via `EnterContext` (Submit path), which counts
  `ContextSpawn` but not `ContextResume`. Must yield to generate resume/scheduler activity.
- IO counters (`IoSubmit`/`IoComplete`): the fast-path optimization in Recv/Send means
  socketpair IO with pre-staged data succeeds via direct syscall without going through
  `Handle::Submit`. Tests that need IO counter activity should use operations that cannot
  fast-path (e.g., Accept, or recv on an empty socket).
- Mode 2 tests: `ProbeCount()` triggers `InitSites()` and should return > 0 when the binary
  has instrumented code. Counters are zero until `Enable()` is called.

## Adding New Probes

1. Add the counter to the `Counter` enum in `counters.h` (before `COUNT`)
2. Add the name string to `CounterName()` in the same position
3. Insert `COOP_PERF_INC(counters_ref, perf::Counter::NewCounter)` at the probe site
4. The counters reference is typically `m_perf` (cooperator member) accessed via
   `GetPerfCounters()`, or `Cooperator::thread_cooperator->GetPerfCounters()` for code
   that doesn't have a direct cooperator pointer
