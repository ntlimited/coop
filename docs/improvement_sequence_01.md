# High-Performance Idiom Extension: Registered Resources, Armed Accept, Parser Windows

The plan for extending coop's idioms to maximum-performance io_uring usage — registered
buffers and files, multishot accept, declared server sharding, and the parser/buffer-ring
meeting point — grounded in an eight-lens research-and-audit sweep (academia, runtime
libraries, production systems, kernel API map; io core, buffer path, socket lifecycle,
idiom stress) whose load-bearing claims were then adversarially verified against this
repo and host before entering this document.

## Calibration: what the evidence says

1. **The architecture already captured the multiple.** The VLDB io_uring-for-DBMS ladder
   (Jasny et al.): cooperative fibers + submission batching account for the 11x over
   synchronous IO; registered buffers add ~11% on top; NVMe passthrough and IOPOLL add
   ~20% each but require raw block devices / socket-free rings. coop IS the
   fibers+batching design — registration work is tail refinement, sequenced after
   correctness and measurement.
2. **Registration taxonomy.** Class A (register once, no userspace lifecycle — the ring
   fd): universally adopted; coop has it. Class B (register at init, sub-allocate in
   userspace — buffer arena, pbuf rings): implemented by helio/folly/Dragonfly and
   shipped OFF by default by all three. Class C (per-connection churn — direct
   descriptors): implemented by two production systems, disabled by both (kernel
   socket-accounting leak, liburing#1192, closed without a linked fix). coop's posture:
   build B behind opt-in config with probes AND version floors; defer C with named
   re-entry conditions.
3. **Registered files are not a throughput feature.** Peer-reviewed at noise — the VLDB
   study collapses Default/RegRing/RegFDs into one indistinguishable bar. coop's own
   ring-fd win (~15ns/enter) is a CLONE_FILES hot-fd contention effect, not the generic
   case. The honest pitch for a fixed-file table is correctness/isolation — a private fd
   space that cannot be closed out from under an in-flight op — plus being the
   prerequisite for (deferred) direct descriptors.
4. **On this host, registered buffers help exactly two legs**: disk READ_FIXED/
   WRITE_FIXED and SEND_ZC-with-fixed-buf. Registered-buffer recv does not exist in any
   released upstream kernel (verified through v7.1) — the socket read path uses provided
   buffer rings (multishot, with a ~13KiB message-size ceiling before single-shot recv
   overtakes it) or caller-owned buffers. Registered buffers and pbuf rings serve
   opposite directions and must not share a config axis; `sqe->buf_index` and
   `sqe->buf_group` are one u16 union field, so setting both is silently wrong (debug
   assert material).
5. **Registration is the most-exploited io_uring surface** (CVE-2023-2598 registered
   buffers, CVE-2022-29582 fixed-file table, CVE-2024-0582 pbuf ring; Google disabled
   io_uring fleet-wide off the back of kCTF submissions). Every registration feature
   stays opt-in and off by default, and capability probes are insufficient — a probe
   succeeds on a vulnerable kernel — so per-feature minimum-kernel floors accompany
   probes, kept separate from liburing-header gates (liburing 2.6 lacks several
   constants entirely).
6. **The published critique of coop's shape** (Pestka et al.: inline user work on a
   polling ring collapses IOPS) is answered by scope: the collapse is an SQPOLL/IOPOLL
   property; coop's default donates no polling core. Covenant to document: SQPOLL is a
   core-donation trade that must never become the default, and a cooperator running
   meaningful compute between Polls forfeits any SQPOLL win. Stackful contexts with
   pooled stacks answer their coroutine-heap objection; work::Grid/Shed answers the
   one-thread ceiling.

## Correctness gates — LANDED with this document

All were adversarially verified before fixing; two claims survived with corrected
mechanisms, which the fixes reflect.

- **G1** Descriptor::Close()/Release() now unregister before closing/releasing. Verified
  failure mode (empirically demonstrated, not the audit's original claim): IORING_OP_CLOSE
  rejects IOSQE_FIXED_FILE with -EBADF and Handle::Submit rewrites SQEs onto the fixed
  index, so Close() on a registered descriptor "succeeded" while the fd never closed —
  permanent fd leak plus a live IO path through the slot. Zero reachability today
  (Registered is used only by bench_io), but it is exactly the proxy opt-in path.
- **G2** RequestLine/ResponseLine staleness detection: Compact() bumps a buffer epoch;
  memoized GetRequestLine()/GetResponseLine() re-calls after compaction assert in debug
  and return null in release instead of serving views into reused memory. Rebase is
  impossible — Compact discards the request-line bytes. The contract (copy method/target
  before header iteration; forward reason before headers) is documented on both APIs.
  Highly reachable for S3-shaped traffic: SigV4 header blocks routinely exceed the 2KB
  recv buffer. Prerequisite for pbuf chunks ever reaching the parser.
- **G3** RunServer/RunTlsServer return bool with a checked, factored BindListen. Verified
  release failure mode was worse than the audit claimed: bind(443) EACCES compiled out,
  then listen() autobound an ephemeral port — a healthy-looking server unreachable on its
  configured port with no diagnostic.
- **G4** io-wq worker caps as opt-in UringConfiguration (insurance-grade: verified that
  coop has no path into the unbounded pool — sockets ride poll-arm, IOSQE_ASYNC is never
  set — but the bounded pool defaults to min(sq_entries, 4*cpus) PER RING and coop is
  one ring per cooperator).
- **G5** tests/test_registered.cpp: registered-descriptor IO, explicit-Close really
  closes, Release unregisters, exhaustion degrades to plain fd.
- **G6** Survey doc corrected: the "4.8x" attribution was false (the paper's 3.4-3.5x is
  RegBufs+passthrough+IOPOLL combined; RegBufs alone ~11%); no-registered-recv trap
  recorded.

## Measurement before machinery

- M1: io-wq punt counter in perf/ + Init-time sysfs probes (max_hw_sectors_kb,
  nr_requests, max_segments), with per-op disk transfer capped at
  min(max_hw_sectors_kb, 512KiB). The three silent io-wq cliffs (oversized transfers,
  full block-layer queues, segment overflow) become visible instead of latent.
- M2: docs/deployment_tuning.md — block scheduler "none" for the cache tier (14-57%),
  qdisc/socket-buffer/IRQ-affinity checklist, RLIMIT_MEMLOCK/NOFILE guidance, and the
  per-feature kernel-floor table. Highest value per unit effort in the entire plan, and
  it prevents host effects being misattributed to feature work.

## Three parallel tracks

### ACCEPT/LISTEN (no registration required)
- A1: Factor ArmedHandle into ArmedHandleImpl<Derived, Item> — a no-behavior-change
  refactor gated on the existing buffer-ring benchmark; the Cancel+Flash sacred drain
  must not fork into divergent copies.
- A2: ArmedAccept (multishot accept behind the blocking-idiom surface) + SOCK_NONBLOCK
  through the existing accept flags argument, removing the per-accept fcntl pair.
- A3: http::ServerConfiguration { port, backlog, reusePort, shardIndex/Count,
  multishotAccept, maxPendingAccepts, timeout, searchPaths } + io::ListenSocket;
  declared one-listener-per-cooperator REUSEPORT sharding (today's REUSEPORT behavior is
  emergent, not declared).
- Covenant (novel — no surveyed system addresses it): multishot accept has no
  provided-buffer backpressure analog; every queued completion is a real kernel fd. The
  backpressure is the listen backlog, enforced by a bounded pending-accept queue that
  disarms at high water and re-arms on drain.

### BUFFER (registration machinery, all opt-in)
- B1: Fixed-file table modernization: sparse registration via raw IORING_REGISTER_FILES2
  (liburing's io_uring_register_files_sparse silently setrlimit()s process-wide — a
  per-ring helper mutating global state under N racing rings), kernel-owned slot
  allocation (FILE_INDEX_ALLOC), FILES_UPDATE through the ring, IsRegistered(), sticky
  failure latch, registration perf counters.
- B2: BufferArena: one huge-page-aligned slab per process registered as few iovecs,
  userspace sub-allocation, opt-in via UringConfiguration::registeredBufferBytes,
  registration-as-probe with warn-and-degrade (ring-fd precedent). Process-global from
  day one so io_uring_clone_buffers (6.12) drops in without redesign.
- B3: io::FixedBuffer { data, len, index } overloads through the existing op-macro
  generator — same op names, overload resolution picks the fixed path (not
  tokio-uring-style renamed ops).
- B4: The WRITE_FIXED bounce rematch in bench_disk_path (cycles/byte + memory bandwidth,
  VLDB-style cumulative attribution). The splice verdict stands until this rematch says
  otherwise.
- B5: SEND_ZC for large userspace bodies (>= a few KiB; kernel copies silently on
  loopback — verify with REPORT_USAGE), after B2: the one-slab-registered-twice trick
  (pbuf ring and fixed buffers over the same memory, so a received bid IS a send
  buf_index) requires the arena.
- Explicitly out of scope for this pass: arena-backing the context bump heap (pulls
  stack_pool and alloc into the change — complexity budget). Decide on B4 data.

### PARSER (gated on G2 only; the only track that makes pbuf reach a real workload)
- P1: Unify ConnectionImpl/ClientConnectionImpl RecvMore/Compact mechanics (CRTP mixin;
  explicit instantiation already keeps both in .cpp files).
- P2: ParseWindow seam: the parser consumes a window abstraction rather than assuming one
  contiguous, mutable, caller-owned buffer.
- P3: ArmedHandle Retain/Release + timeout/Kill variants on Next().
- P4: pbuf-ring recv in the HTTP parser; multi-bgid (small ring for request heads, large
  for bodies); ENOBUFS growth toward a ceiling; the ~13KiB multishot ceiling documented —
  large-body streaming stays single-shot recv / splice.

## Opportunistic (small, no track)
- IORING_RECVSEND_POLL_FIRST as a per-callsite three-state readiness policy
  (Fastpath / Plain / PollFirst) — the ring-native inverse of the EAGAIN fastpath, for
  call sites where data is known-absent (request/response turnaround). Cheapest measured
  win surfaced by the research (up to 1.5x fewer cycles for RPC shapes).
- YieldBudget: promote the 2MB fairness governor to a named idiom (DESIGN_IDIOMS.md) and
  one semantic wrapper; migrate the tcp_proxy example's unfenced relay loops.
- rw_flags plumbing in READ/WRITE op args now; RWF_DONTCACHE behind a 6.14 probe later.
- Adaptive submission batching in Uring::Poll keyed on m_pendingOps vs yielded-context
  count (VLDB's adaptive batching; the counters already exist).

## Deferred, with named re-entry conditions
- **Direct descriptors**: re-enter when (a) the liburing#1192 kernel socket-accounting
  leak has a confirmed fix, (b) the DataPath selector exists (see below), and (c) the
  host has 6.8+ FIXED_FD_INSTALL as an escape hatch. Even then: long-lived descriptors
  only (listeners, upstream pool, cache container files) — never churned per-connection
  sockets. TlsTransport is real-fd permanently (SSL_set_fd, kTLS TCP_ULP setsockopt).
  Today's arithmetic: ~10-20ns fget/fput saved vs ~500ns fastpath lost on exactly the
  keep-alive shape — a net loss for connection sockets.
- **IOPOLL**: requires a second, storage-only ring per thread — ASYNC_CANCEL is not
  iopoll-able, which breaks the sacred Handle destructor drain on such a ring, and fsync
  cannot be issued there at all. HYBRID_IOPOLL (tolerable for a shared thread) is 6.13.
  The second-ring constraint is recorded here so the disk tier is shaped for it later.
- **Future-kernel tier** (design toward, not around): NAPI busy poll (6.9), incremental
  pbuf consumption (6.12), clone_buffers (6.12 + liburing 2.8), PBUF_STATUS (6.8),
  zcrx (6.15, and it requires DEFER_TASKRUN — which coop measured 20-30% slower; that
  regression must be understood first).

## New idioms to name in DESIGN_IDIOMS.md
- **Registration lifecycle with kernel resource tags**: an RAII region/slot lease whose
  teardown is kernel-notified (a tagged unregister posts a CQE at quiesce), converting
  unregister-while-in-flight from a hazard into a completion event. The syscall layer
  has supported this since 5.13; no surveyed runtime exposes it — genuine
  innovate-beyond territory.
- **Sticky registration latch**: the first registration failure disables the feature for
  the ring's lifetime (folly precedent) — a probe that can also trip at runtime.
- **YieldBudget**: the fairness governor as a named, reusable semantic.
- **DataPath selector**: per-transport/per-descriptor engine choice (SyscallSplice /
  RingFixed / Bounce) replacing booleans that cannot express three-way trades — the
  resolution to the direct-descriptors-vs-splice conflict.
- **Backpressure covenant**: every armed (multishot) op names its backpressure mechanism
  at design time — pbuf pool exhaustion for recv, bounded queue + listen backlog for
  accept.

## Config surface
- UringConfiguration: ring-scoped mechanisms — registeredSlots (exists),
  registeredBufferBytes (B2), iowq caps (landed), buffer-ring geometry (exists),
  per-feature floors/probes.
- CooperatorConfiguration: nothing new; registration is ring-scoped.
- http::ServerConfiguration (A3): application-scoped topology — shards, backlog,
  multishot, pending-accept bound.
