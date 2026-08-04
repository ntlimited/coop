# Batteries, Comforts, and First-Class Idioms — Peer Library Survey (2026-08)

What peer ecosystems ship that coop does not, surveyed across three lenses — Seastar +
folly (C++ performance-first), Boost.Asio/Beast/Cobalt + libuv + libevent/libev (C/C++
mainstream async IO), and tokio + the Go stdlib (the developer-ergonomics gold
standards) — each verified against primary docs/source and against coop's actual tree
(HEAD bf3ffd5), not against assumptions. Items coop already has (channel toolkit with
select/merge/ticker, WebSocket, context-locals, epoch reclamation, work stealing, the
full io_uring surface, perf counters + CPU sampler + dashboard) were excluded from gap
lists by construction.

## The six mechanisms behind "feels productive"

Every concrete item below instantiates one of these (the tokio/Go lens's meta-finding):

- **A.** One value carries ambient request policy — deadline + cancellation + cause —
  as an explicit argument that survives crossing into code you don't own (Go `context`).
- **B.** One uniform "handles a request" interface, making middleware an algebra
  (`http.Handler`, `tower::Service`).
- **C.** Introspection batteries at the edges — look inside a running system without
  having instrumented it (pprof/expvar, tokio-console).
- **D.** Time as an injectable dependency, not a syscall — the precondition for fast,
  deterministic tests (synctest, `tokio::time::pause`, Seastar `manual_clock`).
- **E.** Structured concurrency as a value you hold, not a discipline you maintain
  (errgroup, JoinSet, gate).
- **F.** Cancellation contracts documented per-API.

## What coop already wins — document these as differentiators

Stackful contexts delete entire bug classes peers pay for continuously:

- **No cancel-safety hazard.** tokio's subtlest bug class (dropping a `read_exact`
  future loses buffered progress) cannot occur — a blocked op keeps its stack.
- **Real stacks.** folly's entire AsyncStack subsystem and gdb fiber scripts exist to
  recover what coop has natively; a stall backtrace here is a *real* backtrace.
- **No span-propagation hazard.** tracing's #1 gotcha (spans don't cross spawn; enter()
  across await lies) vanishes — a ContextVar-held span is correct by construction.
- **No callback-cascade stack overflow** (libevent forces deferred callbacks for this).
- **Backpressure by blocking.** A context that can't send doesn't proceed — the
  property tower encodes as `poll_ready` and Node as `'drain'` events.
- **Kill trees are the drain registry** every peer makes users hand-build.
- **Ring-native DNS** (no getaddrinfo thread pool — no peer has this) and a CPU
  sampler (Seastar has none).
- **Typed `ContextVar<T>`** beats `ctx.Value(any)` on every axis.

## The unanimous core (independently top-ranked by 2–3 lenses)

### 1. Cancellation & deadline upgrade — mechanism A, the #1 across all lenses
coop's kill is Asio's `terminal` grade with no cause and no budget. The composite:
- **Deadline inheritance**: `Context` carries an absolute deadline; children inherit
  `min(parent, requested)`; every blocking op clamps its timeout against
  `RemainingBudget(ctx)`. One 30s budget at the request root covers DNS + connect +
  TLS + write + read + upstream — today each stage gets its own fresh 30s.
- **Cause**: `WhyKilled()` (client-gone / deadline / draining / admin) — it picks the
  status code (499/504/503) and the metric bucket. `IsKilled()` stays.
- **Graduated cancellation** (Asio): `terminal`/`partial`/`total` on `Handle::Cancel` —
  io_uring can express "cancel this recv and report the partial byte count" natively.
- **`OnKill(ctx, fn) -> stop`** (Go `context.AfterFunc`): generalizes
  ShutdownOnKillGuard; also a per-context LIFO cleanup registry (`t.Cleanup` shape).
- **Escalating deadline idiom** (Boost.Process): soft (graceful) then hard (terminal) —
  the shape of every drain policy.

### 2. Structured concurrency scope — mechanism E
`coop::Group`: RAII scope; `Go(fn)` spawns under it; destructor/`Wait()` joins all;
first failure kills siblings (fail-fast `join`) or collects everything
(`gather`/`collectAllTry` — both named, per Cobalt/folly, because users hand-roll the
distinction wrongly); `SetLimit(n)` for bounded fan-out (`max_concurrent_for_each`).
Complementary to kill trees: kill propagates termination down; a scope drains upward
and refuses new work (Seastar `gate`'s close/check/holder shape).

### 3. Coordination batteries
- **Semaphore** with RAII units, movable into spawned children (tokio
  `acquire_owned`), timeout + kill overloads — the substrate for per-upstream and
  global in-flight caps and for `Group::SetLimit`. The single most-cited primitive.
- `Once`/`OnceValue`, coop-aware `RWMutex`, `Cond` (predicate + timeout), and a
  **shared/broadcast future** (single-flight request coalescing — cache stampede
  control for the S3 proxy).
- **watch-style latest-value channel** (config/credential rotation — SigV4 rollover).

### 4. Graceful drain — the composite every peer makes users build
`Server::Drain(deadline)`: pause the acceptor (evconnlistener_disable-equivalent —
coop currently has no lever), flip keep-alive responses to `Connection: close`, wait
on live connections, then kill the residue at the hard deadline. Plus **unref/daemon
semantics** (libuv): a `SpawnConfiguration` flag so Grid stealers and watchers never
hold liveness — orthogonal to kill trees and required for drains to converge. Go's
`Shutdown(ctx)` + `RegisterOnShutdown` is the reference surface; REUSEPORT handoff
(already half-built) is the zero-downtime-restart story to document.

### 5. Router + middleware seam — mechanism B
Method + wildcard routing (Go 1.22 ServeMux: `"GET /{bucket}/{key...}"`,
most-specific-wins, `PathValue()` as string_views into the existing buffer) and a
handler seam for cross-cutting layers (deadline, request-id, access log, concurrency
limit, load-shed) — today's `void(*)(ConnectionBase&)` admits neither. tower's lesson:
build the chain at startup, resolve per route, never per request; ordering is
semantic. Copy `httputil.ReverseProxy`'s Rewrite/SetXForwarded hop-by-hop hygiene
verbatim — it is a solved security problem.

### 6. Testing & virtual time — mechanism D
- **Clock seam now, not later** (Seastar's lesson: manual_clock works only because
  Clock is a parameter throughout — "a design decision with a deadline"). Then the
  synctest/auto-advance rule: an idle cooperator with no in-flight IO jumps to the
  next timer deadline. `tests/test_timer.cpp` becomes instant and deterministic.
- **In-memory duplex transport** (tokio `duplex`, bufferevent_pair) + a recording
  connection — HTTP/WS/TLS tests without sockets (`httptest` shape).
- **Deterministic scheduling**: coop already owns "who runs next"; exposing it as a
  pluggable seeded function is DeterministicSchedule-grade race reproduction almost
  for free — far cheaper than folly needed.
- **Fault seams**: alloc-failure injection (`fail_after(n)` + the exhaustive loop
  idiom) and an io_uring op-level fault layer (`-ENOBUFS`, short reads, `-ECANCELED`
  made systematic). Longer term: turmoil-style network sim (needs the clock seam).
- **TSan fiber annotations** (`__tsan_*_fiber` around the switch core) — the race
  detector for Grid/deque/epoch.
- **Reactor-aware microbenchmark harness** with per-iteration hardware counters
  (Seastar perf_tests + linux_perf_event) — gbench can't measure suspend/resume.

### 7. Operational surface — mechanism C
- **Prometheus text exposition** over the existing counters — the last mile that makes
  the dashboard credible to ops (Seastar has it; folly doesn't).
- **Stall detector** (Seastar): `timer_create(CLOCK_THREAD_CPUTIME_ID)` per cooperator
  thread; if the yield counter hasn't moved, log "stalled for N ms" + a real
  backtrace. The characteristic cooperative failure made visible; complements the
  sampler (where time goes vs when a budget blew). stall-analyser-style aggregation
  after.
- **Idle/busy loop metric** (uv_metrics_idle_time) — the one loop-health number the
  counters don't derive; sample from a prepare-style hook.
- **Per-request live view**: spans as ContextVar structs feeding `/api/status` —
  tokio-console assembled from parts already shipped.
- **cgroup-aware topology** (`uv_get_constrained_memory`, affinity-aware parallelism)
  — NUMA pinning and pool sizing are wrong in containers reading host numbers.
- pprof/expvar-shape endpoints on the status server.

### 8. Traffic policy (client + server)
- **Token bucket** with libevent's group semantics + `min_share` (the non-obvious
  fairness fix: rotate usable shares randomly rather than fair-but-useless 1-byte
  slices) behind Beast's `RatePolicy` interface shape (query-cap / report / tick;
  0 = wait for next tick). Template parameter ⇒ zero cost when unlimited.
- **Retry with jittered exponential backoff + a shared retry budget** (tower
  `retry::budget`'s deposit/withdraw model — the anti-retry-storm design). Hard rule:
  retries draw from the same request deadline. Note: *no* surveyed ecosystem ships a
  circuit breaker — unclaimed territory, consciously left to the mesh layer by most.
- **Load-shed inputs, not policy**: expose queue depth / wait-in-queue / RTT via perf
  counters; CoDel/adaptive-concurrency belongs to the application ("trust developers").
- **EMFILE spare-fd trick** (libuv) in the accept loop — today an EMFILE is a hot spin.

## Gaps by module (the batteries list)

- **io**: `io::Relay` (bidirectional copy — tcp_proxy is 231 hand-rolled lines);
  typed socket-option surface (`SetOption(opt::NoDelay{true})`, DEFER_ACCEPT, buffer
  sizes) replacing scattered setsockopt; `io::Random` (getrandom; WS masking keys);
  chainable-deleter buffer (Seastar `deleter` / IOBuf FreeFunction+userData — the
  unlock for pbuf-ring entries flowing through HTTP/TLS layers as normal buffers);
  child processes via `pidfd_open` + `IORING_OP_WAITID` (ring-native child exit — no
  SIGCHLD, no reaper races; take libuv's exec-errno-pipe verbatim) + SCM_RIGHTS fd
  passing for listener handoff; inotify watcher (config reload; statx fallback is
  ring-native where libev's blocks); signalfd delivery to a context — replacing the
  sigaction+watcher-thread shutdown path — with `IORING_OP_MSG_RING` fan-out to
  interested rings as the innovate-beyond design (kernel-side fan-out, no
  async-signal-safe locking; strictly cleaner than libuv's self-pipe tree).
- **DNS**: AAAA/dual-family, TCP fallback on truncation (silent correctness hole
  today), 0x20 case randomization, search domains, caching, distinguishable DNS
  errors on `Connect(host)`. Then **Happy Eyeballs (RFC 8305): implemented by NO
  surveyed C/C++ library** — verified — and nearly free on coop
  (`CoordinateWith` over staggered Connect handles). Genuine differentiator.
- **HTTP/WS**: permessage-deflate; WS idle-timeout + half-interval keep-alive pings;
  `detect_ssl` (one port serving plaintext+TLS); header-size cap (DoS hardening,
  configurable); URI toolkit (percent-decode, query split — security-sensitive,
  borrow evhttp's test vectors); surface `somaxconn` clamping of the backlog; audit
  keep-alive detection against Beast's exact 1.0-vs-1.1 Connection rules; buffer
  stability documentation table (Beast's per-type invalidation contracts).
- **Time**: keyed DelayQueue API over the existing pairing heap (per-connection idle
  timeouts without a sleeper context each); wall-clock/periodic timers (`ev_periodic`
  — drift-free "hourly on the hour", structurally inexpressible on a monotonic-only
  queue); logical-operation deadlines (Beast tcp_stream `expires_after` spanning a
  whole request's ops) — subsumed by core item 1.
- **App scaffolding** (deliberately thin, coop-as-library): config overlay from
  env/string over UringConfiguration/CooperatorConfiguration (Asio's config shape —
  makes benchmark sweeps and container deploys tractable; includes knobs like
  io_uring_iowait); runtime-mutable tunables with change callbacks (folly settings
  shape) — optional; a `sharded<T>`-style per-cooperator service helper is the one
  Seastar scaffolding piece worth considering when multi-cooperator apps arrive.
- **Stacks**: guard pages (sampled, mprotect + SIGSEGV) turning overflow into a fault;
  magic-fill high-water recording so users size stacks empirically; frame-pointer
  stitching across the switch boundary + gdb scripts to walk suspended contexts.
- **Distinctive/novel**: `BatchDispatcher` (N contexts add IDs, one backend query —
  exploits FIFO scheduling; very coop-shaped); `execution_stage` (call batching for
  i-cache locality); folly `result`/`epitaph`-style opt-in error-context accumulation
  if checked returns ever feel bare.

## Explicit non-adoptions (with reasons)

- Beast's message/fields model and evbuffer-as-container — coop's zero-copy pull
  parser and kernel splice are better fits ("do no work nobody asked for",
  contiguous layout); steal only the EOL taxonomy, freeze-as-assertion, and
  stability-contract documentation.
- Asio's executor/property model — abstraction over backends coop doesn't have.
- Error-string tables, portability shims, serial ports — Linux-native by design.
- Deferred-callback machinery — the problem doesn't exist stackful.
- Adaptive concurrency/CoDel as library policy — expose inputs, let apps decide.
- Thread-pool getaddrinfo — ring-native DNS is already the better design.
- Full RPC framework (Seastar) — the S3 proxy speaks HTTP; revisit only with demand.

## Suggested sequencing

Tier 1 (substrate, unlocks the rest): deadline/cause/OnKill on Context ▸ Semaphore ▸
Group/gate ▸ drain + unref + acceptor pause. Tier 2 (operational credibility):
router+middleware seam ▸ Prometheus exposition ▸ stall detector ▸ idle metric ▸
token bucket/retry budget. Tier 3 (testing): clock seam + virtual time ▸ in-memory
transports ▸ fault seams ▸ deterministic scheduling ▸ TSan fibers. Batteries
(independent, as needed): Relay, sockopts, DNS parity, URI, WS deflate, DelayQueue,
guard pages, signalfd/MSG_RING, pidfd children, Happy Eyeballs.
