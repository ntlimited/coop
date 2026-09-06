# Debugging suspended contexts with GDB

A native thread backtrace only shows the context currently using that thread. To see work parked
on a coordinator or waiting for I/O, load the coop helper into a stopped process:

```text
(gdb) source /path/to/coop/tools/coop_gdb.py
(gdb) coop contexts
(gdb) coop bt 0x12345678
(gdb) coop bt all
```

Use a numeric **Context address** from `coop contexts` with `coop bt`. Names are labels; addresses
identify the objects in this snapshot and can be reused after those objects die. The listing includes
state, parent address, and owning cooperator address. No runtime instrumentation or application API
changes are needed.

The helper reads target memory and debug information. It never invokes inferior functions, changes
registers or memory, switches selected threads/frames, or resumes execution. Stop **all** threads
first; non-stop inspection while another thread is running is rejected. List mutations or context
switches caught halfway through can still produce incomplete state in a stopped process. The helper
reports readable partial results and does not acquire runtime locks.

## Build requirements and limits

Use a GDB build with Python support and matching debug information for coop and your executable
(`-g`, or CMake `Debug` / `RelWithDebInfo`). Compile code whose frames you want to inspect with
`-fno-omit-frame-pointer`; `-fno-optimize-sibling-calls` retains tail-call frames. Inlining can still
remove named functions. Use `set substitute-path OLD NEW` for relocated source trees.

`coop bt` decodes the saved register layout in `coop/detail/context_switch.S`, then walks frame
pointers within the context's stack segment. It prints addresses, symbols, and source lines where
available. This is a saved-stack trace, not a full DWARF unwinder: it does not reconstruct locals,
arguments, inline frames, signal frames, or tail calls. Missing frame pointers, unreadable memory,
invalid bounds, or a damaged chain produce an explicit truncation message. A normal trace stops at
`CoopContextEntry`. Symbolization uses return-address call sites.

x86-64 System V and AArch64 saved layouts are supported. The GitHub smoke test currently runs on
x86-64; AArch64 execution is not covered by that runner. PAC-signed return addresses and other ABI
extensions are not decoded.

A **running context's saved registers are stale**. For it, use GDB's native `thread apply all bt`,
then select the relevant native thread for ordinary frame and variable inspection. A context still
launching has no suspended stack to inspect. An interruption within the assembly context switch
also needs native thread inspection.

Traversal is bounded to 4,096 combined registry/context nodes, 128 frames per trace, 128 bytes per
name, and a maximum validated stack size of 1 GiB. Names are escaped. The shared bump heap's current
watermark tightens the stack's lower bound. These limits keep damaged snapshots from causing
unbounded walks; they do not prove that a plausible-looking frame is valid.

## Core files

Use the exact executable and matching shared libraries/debug files that produced the core:

```sh
gdb /path/to/application /path/to/application.core
```

Then source the helper and run the same commands. Core inspection does not require a running
scheduler or a functioning diagnostic endpoint. A core that omitted stack mappings or an executable
without the necessary type information cannot provide a complete trace.

To capture a stopped process with GDB, use `generate-core-file /path/to/application.core`.
This is a separate explicit debugger operation; none of the coop commands creates files.

## Repeatable smoke test

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target coop_gdb_fixture -j
python3 tests/gdb_smoke.py build/debug/bin/coop_gdb_fixture
```

The fixture starts a real cooperator, parks a child in a named call chain, and stops at a marker
while its parent is running. The test checks registry enumeration, parent/state reporting,
symbolized blocked stacks, running-context handling, and lack of changes to the selected
thread/frame, PC/SP, and parked stack. It repeats the checks after generating and reopening a core.
GDB must be permitted to debug child processes and generate cores.

For hosts that disallow `io_uring`, add `--bare`. This mode constructs real Context objects and
uses the real `ContextInit` and assembly switch to park a stack, without creating a cooperator.
It checks saved-stack inspection and invalid-SP handling, but cannot validate populated registry
enumeration or scheduler integration. Neither mode substitutes mock type layouts.
