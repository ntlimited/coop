# Blocking IO Shutdown 01

This note captures the shutdown contract for coop's blocking IO and the
required pattern for coop-owned long-lived loops.

## Problem

coop currently has two competing expectations:

- Blocking descriptor APIs are intentionally simple and cleanup-friendly.
- Long-lived service loops still need prompt shutdown.

Treating all blocking IO as implicitly kill-aware would change semantics,
introduce overhead on the common path, and make post-kill cleanup IO harder.
Treating all blocking IO as kill-oblivious is workable, but only if coop-owned
loops use an explicit shutdown pattern instead of assuming `IsKilled()` alone
will wake a blocked `Accept`/`Recv`.

## Decision

- Plain blocking IO remains non-kill-aware by default.
  `io::Accept(desc)`, `io::Recv(desc, ...)`, `io::Read(desc, ...)`, and friends
  continue to block until completion or timeout.
- The generic kill-aware pattern is the blocking `*Kill` family:
  `io::AcceptKill(desc)`, `io::RecvKill(desc, ...)`, `io::PollKill(desc, ...)`, and
  friends. These wrappers are generated uniformly across the macro-defined operation surface.
  Custom helpers with internal readiness waits should mirror that contract with an explicit
  `*Kill` sibling instead of silently changing the default blocking behavior.
- Explicit async-handle composition remains the lower-level escape hatch:
  create an async `io::Handle`, submit the operation, then wait with `CoordinateWithKill`.
- coop-owned long-lived loops must use an explicit shutdown wake path.
  Accept loops should use `AcceptKill` or equivalent async-handle composition.
  Read loops may use the same pattern, a finite timeout, or a deliberate
  "close from another context" strategy, but they must not rely on `IsKilled()`
  alone to wake blocking IO.

## Rationale

- Keeps the cheap, unsurprising blocking API for ordinary cleanup and leaf code.
- Avoids forcing kill bookkeeping onto every blocking IO wait.
- Makes kill sensitivity an explicit callsite choice instead of hidden library
  policy.
- Matches coop's existing "compose your own semantics" bias.

## Coop-Owned Usage Rules

- Listener loops in `coop/http/` and examples must use async `Accept` +
  `CoordinateWithKill` or the generated `AcceptKill` wrapper.
- If a coop-owned loop uses blocking `Recv`/`Read`, it must also provide an
  explicit wake strategy:
  finite timeout, a generated `*Kill` wrapper, or a socket-level shutdown/close
  strategy armed from another context.
- Documentation must state plainly that `Handle::Wait()` is not kill-aware.

## Test Contract

- Add a regression test proving that an accept loop built from async `Accept` +
  `CoordinateWithKill` exits when killed without any inbound connection.
- Existing shutdown tests remain valid for yield/coordinator/signal/sleep paths,
  but they are not sufficient coverage for listener loops.

## Non-Semantic Follow-Ons

These are independent runtime fixes and do not depend on the API decision:

- scheduler no-runnable wait path should sleep instead of spinning
- perf sampler ring publication should use a consistent published-count view
- shutdown helper should use a signal-safe wake primitive without pipe-fill risk
