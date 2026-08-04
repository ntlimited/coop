# Zero-Copy Body Data Paths — Industry/Kernel Survey (2026-08)

External research supporting coop's disk data-path work (`ReadBodyToFile` /
`SendBodyFromFile`, `io::SpliceToFile`, `PipePool`) and the buffer/socket management
roadmap. Kernel-source claims below were verified against `torvalds/linux` master at
research time; items flagged unverified should be confirmed before entering a design doc.

## Headline findings

1. **`IORING_OP_SPLICE` is unconditionally punted to an io-wq worker thread**
   (`REQ_F_FORCE_ASYNC` in `io_uring/splice.c`, no conditions). Every splice SQE costs a
   thread handoff — strictly worse than the `splice(2)` syscall for a thread-per-core
   design. Netty measured io_uring-splice ~6k RPS vs ~7.2k for classic `sendfile()` on
   1 MB files (netty#15747). coop's `Splice`/`SpliceToFile` use the syscall + cooperative
   Poll — the right variant; keep it that way.

2. **splice socket→pipe→file was never as zero-copy as advertised.** The pipe→file leg
   (`iter_file_splice_write` → `write_iter`) copies pipe pages into the page cache for
   buffered writes; the socket→pipe leg copies the *linear* skb head (`linear_to_page`) —
   only page frags are truly referenced. Same syscall count as recv+write, at best one
   copy saved. Whether splice beats a ring-native bounce (recv into registered buffers +
   `WRITE_FIXED`/`RWF_DONTCACHE`) is an empirical question for our benchmark harness —
   the `ReadBodyToFile` idiom hides the engine, so the answer swaps in freely.

3. **Filesystem choice is load-bearing**: in current mainline, XFS sets
   `FOP_BUFFER_WASYNC` (io_uring buffered writes run inline); ext4 does not — every
   buffered write punts to io-wq *and* serializes per inode (hashed by inode). Target XFS
   for the cache tier, 4K blocks (ScyllaDB's 1K-block incident cost most of 14.5 GB/s).

4. **`RWF_DONTCACHE` (kernel 6.14) resolves the O_DIRECT vs page-cache argument** for a
   caching proxy: data rides the page cache (coherency, readahead, write combining, no
   alignment rules) but pages drop once IO completes. Axboe: 65% faster at half the CPU
   on a thrash benchmark. Per-IO policy: large streaming fills use it, small hot objects
   stay buffered. O_DIRECT + registered buffers is *not* the ceiling it once looked
   like here: in the VLDB io_uring-for-DBMS study (Jasny et al.) registered buffers
   alone are worth ~11% end-to-end; the paper's headline 3.4–3.5× scale-out multiple
   (3.51× read / 3.37× write) is RegBufs + NVMe *passthrough* + IOPOLL combined, and
   passthrough needs raw block-device access while IOPOLL needs a socket-free ring —
   neither applies to a filesystem-backed cache tier. (An earlier revision of this doc
   claimed "up to 4.8×"; that number is not in the paper.)

## Trap list (verified)

- **There is no registered-buffer recv.** No released upstream kernel (through v7.1)
  accepts `IORING_RECVSEND_FIXED_BUF` on plain RECV — the socket *read* path uses
  provided buffer rings or caller-owned buffers, permanently for planning purposes.
  Registered buffers help exactly two legs on this host: disk READ_FIXED/WRITE_FIXED and
  SEND_ZC-with-fixed-buf. Registered buffers and pbuf rings serve opposite directions and
  must not share a config axis (`sqe->buf_index` and `sqe->buf_group` are the same u16
  union field — setting both is silently wrong).

- **`pipe_user_pages_soft` cliff**: an unprivileged UID gets 1024 pipes at 64 KiB;
  every pipe after that is silently created with 8 KiB capacity — an 8× throughput cliff
  visible in no error path. Accounting is per-UID across processes. coop's `PipePool`
  caps at 8 pipes per uring — safely under for sane thread counts; revisit if pipe use
  ever scales with connections (HAProxy's cap is maxconn/4 for comparison).
- **EAGAIN ambiguity in splice**: EAGAIN means "socket empty" *or* "pipe full", and the
  kernel doesn't say which. HAProxy's answer: if the pipe holds data, treat as
  wait-for-room, else wait-for-input. Our two-phase SpliceToFile drains the pipe fully
  each round, sidestepping the ambiguity — preserve that invariant.
- **splice from kTLS-RX sockets works and yields plaintext**, but any non-data record
  (handshake, alert, KeyUpdate) returns `-EINVAL` — which is also the generic "splice
  unsupported" errno. Not fatal: fall back to recvmsg for that record (HAProxy's
  raw_sock.c documents this exactly). Enables a future TLS splice fast path.
- **`sync_file_range(2)`**: its own man page says not to use it. **`POSIX_FADV_DONTNEED`**
  won't free dirty pages and ignores unaligned ranges — not a drop-after-write mechanism.
- **SEND_ZC below a few KB is noise** (+0.4% at 600B, +22% at 4000B, Begunkov's numbers)
  and silently falls back to copy on loopback. Gate by size; verify with
  `IORING_SEND_ZC_REPORT_USAGE`.
- **io-wq thread explosion**: `IOSQE_ASYNC` on 4096 requests → 4096 worker threads
  (Cloudflare). Set `IORING_REGISTER_IOWQ_MAX_WORKERS` explicitly.
- **Strategic**: Torvalds is openly hostile to splice/sendfile these days ("I kind of
  hate it"; sendfile "a mistake") and the kernel community is pressuring their zero-copy
  semantics. Keep every splice path behind a toggle with a working bounce fallback —
  which the transport-spliceability design already gives us.

## Patterns worth copying

- **nginx `sendfile_max_chunk`** (default 2 MB since 1.21.4): cap bytes per zero-copy op
  so one fast connection can't seize the worker. Same failure mode exists for coop's
  `Sendfile`/`ReadBodyToFile` loops on a cooperative slot — add a per-iteration cap +
  yield. *(Open follow-up.)*
- **HAProxy splice engagement heuristics**: 4096-byte minimum before pipes are worth it;
  engage only after 3 consecutive full-buffer reads ("streamer" detection); disengage
  after 1s idle. Adopt for any auto-splice policy.
- **nginx header+body coalescing**: TCP_CORK around writev(headers) + sendfile;
  HAProxy uses per-call MSG_MORE instead. No credible published measurement of the win —
  benchmark before building cork state machines.
- **Cache durability consensus** (nginx, ATS, Squid, Kafka, BlueStore): cache content is
  regenerable — no fsync on the hot path. Self-validating entries (payload, then
  checksummed length trailer written last), temp + same-filesystem rename for namespace
  atomicity, discard-don't-repair on recovery. ATS goes further: cache raw devices, an
  append-only circular write cursor, "cache data on disk is never updated", 10-byte
  in-RAM directory entries.
- **File-per-object doesn't survive at scale** — every mature cache converged on
  container files / raw devices with an application-level index.
- **One SO_REUSEPORT listener per thread** with its own multishot accept, not N rings
  racing one shared listen fd.
- **Grow provided-buffer pools on ENOBUFS** rather than merely refilling.

## Benchmarks we owe ourselves (priority order)

1. `SpliceToFile` vs ring-bounce (Recv + `io::Write`) for receive-to-disk at 4K/64K/1M/
   16M bodies — decides the default engine under `ReadBodyToFile`.

   **Answered** (`benchmarks/bench_disk_path.cpp`, TCP loopback, warm page cache,
   interleaved rounds, shared 8-core host under ~12 ambient load — treat as
   directional): splice wins every size class, ~2x wall throughput at 64K-16M and ~5x
   at 4K, at 55-75% less CPU per GiB (e.g. 1M: 4.9-5.3 GiB/s at 0.28-0.32 cpu-sec/GiB
   vs 2.3-2.6 GiB/s at 0.42-0.47). Splice stays the default engine.

   Why the survey's "bounce wins" prediction inverted here: that argument priced the
   bounce as fully-inline io_uring ops with ~0 amortized syscalls. coop's bounce today
   is blocking-style ops — a fastpath recv plus a full ring round trip per `io::Write`
   — so its syscall budget matches splice's two-per-hop while paying both memcpys.

   **Rematch run** (same harness, third engine: recv into a registered BufferArena
   lease + WRITE_FIXED): the fixed-buffer write beats the plain bounce by 8-20% at
   64K-16M (e.g. 64K: 3.0 vs 2.5 GiB/s; 1M: 3.2 vs 2.9) and nothing at 4K — matching
   the VLDB ~11%-class attribution for registered buffers — but splice still wins
   roughly 2x against BOTH bounce variants at every size. Splice is confirmed as the
   default engine under ReadBodyToFile; the registered-buffer path is the right
   substrate for workloads that must land in userspace anyway (TLS bodies, inspected
   payloads) and for the future SEND_ZC leg.
2. Serve-after-fill hit-rate window for real traffic — decides buffered+DONTCACHE vs
   O_DIRECT for the cache tier.
3. MSG_MORE / writev-first-chunk / cork for header+body coalescing.
4. splice(2) pass-through vs in-ring recv+send for the pure proxy relay (HAProxy claims
   2× per core at 10G+; our fastpath batching may close the gap).
5. Does splice pipe→O_DIRECT-file DMA from pipe pages? (Unconfirmed anywhere; if yes,
   socket→pipe→O_DIRECT is a true zero-copy receive path and the strongest argument for
   keeping splice.)

Full per-engine findings (nginx, HAProxy, Varnish, ATS, Envoy, Seastar/ScyllaDB,
io_uring-native) with citations live in the research transcript; the durable claims and
their sources are reproduced above.
