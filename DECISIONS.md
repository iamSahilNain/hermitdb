# Decision Log

Rules (SPEC §6): Claude Code formats this file and keeps Context/Options/
Trade-offs current; **every Resolution is written by the human**, and code may
not embed a resolution before it lands here. Stubs carry `// DECISION-N pending`
markers at the affected sites. "Measured consequence" is filled after benchmarks.

---

## DECISION-1 — epoll: level-triggered vs edge-triggered

**Context.** CP2's reactor registers listener + connection fds with epoll. LT
re-reports readiness while data remains; ET reports only transitions.
**Options.** (a) LT. (b) ET. (c) ET with `EPOLLONESHOT` re-arm.
**Trade-offs.** LT is forgiving: a partial drain just re-fires — but wakeups
repeat for data you've chosen not to read yet. ET wakes once per transition —
fewer syscalls under load, but you MUST drain to EAGAIN or starve the
connection (the classic ET bug), and nonblocking fds stop being optional.
Affects: the recv/send drain loops, EPOLLOUT handling, the slow-reader test.

**Resolution.** **(a) Level-triggered.**

The deciding argument is what each one does when I get it *wrong*. Under LT, a
loop that stops draining early re-fires and finishes the work late — a
throughput bug. Under ET, the same mistake leaves bytes sitting in a socket
that will never be reported again: the connection hangs until the client times
out, and it reproduces only under load. One is a slow path, the other is a
silent stall, and I do not have production traffic to shake out the second.

LT also buys the fairness knob directly. `kMaxReadPerEvent` (1 MiB) lets me cut
off a client mid-drain and move on, knowing the remainder re-fires next tick.
That bound is exactly what makes the slow-reader test pass without special
casing, and under ET it would be the starvation bug rather than the fix.

The syscall argument for ET is real but small here: the drain loops are written
to run to EAGAIN anyway, so the extra wakeups only occur when the fairness cap
actually fires. Redis's own `ae_epoll.c` runs level-triggered, which is decent
evidence the tradeoff holds at scale.

**Measured consequence.** No stalls across the suite: 200 concurrent pipelining
clients (`test_pipeline_clients.py`), and a 5,000-command flood to a client with
a 4 KiB receive buffer that never reads, with a well-behaved client still served
in well under the 5 s bound (`test_slow_reader.py`). Throughput numbers in the
README table are all LT.

---

## DECISION-2 — fsync policy default (`always` | `everysec` | `no`)

**Context.** CP4 appends mutations to the WAL; when the file is fsync'd
decides what an ACKed write survives. It is a `--fsync` flag; this decision is
the DEFAULT, and the durability story told in the README's money row.
**Options.** (a) `always`: fsync per append. (b) `everysec`: background-tick
sync, ≤1s loss window. (c) `no`: kernel decides.
**Trade-offs.** `always` buys real durability at the price of one disk flush
per write (measure it: `bench/run_bench.sh` emits the µs/fsync number) —
group commit can amortize it. `everysec` is Redis's default posture: bounded
loss, near-`no` throughput. `no` is a benchmark toy. The related ordering
question — append before or after the client reply — is part of CP4 and must
be documented here with the crash-window analysis.

**Resolution.** **No silent default — `--wal` keeps requiring an explicit
`--fsync=`. `everysec` is the recommended setting and the one the docs use.**

I considered defaulting to `everysec` and deleting the check. I kept the check.
A durability posture is not a convenience default: the difference between
`always` and `everysec` is whether an `+OK` I already sent to a client is a
promise or a strong hint, and an operator who never typed the flag has not made
that call — they have inherited mine. Making it explicit costs eight characters
once per deployment and removes an entire class of "we thought it was durable"
incident. `no` exists to isolate WAL-write cost from fsync cost in the
benchmark; it is not a serving configuration.

**Ordering (the part CP4 owns).** Append and fsync happen **after the command
mutates memory, before its reply reaches the client**:

```
execute -> mutate memory -> WAL append -> fsync (if always) -> reply
```

Crash windows, per policy:

| crash point | `always` | `everysec` | `no` |
|---|---|---|---|
| before append | write lost; client never got `+OK` — consistent | same | same |
| after append, before fsync | n/a (fsync is inline) | up to 1 s of ACKed writes lost | unbounded, kernel's discretion |
| after fsync, before reply | write survives; client saw a timeout and must retry — an at-least-once duplicate, not a loss | same | same |
| after reply | durable | may be lost (≤1 s) | may be lost |

The reverse order — reply first, then log — is faster by exactly one flush of
client-visible latency and makes `+OK` mean "probably". I would not defend that
to someone whose data it was. The residual honest weakness is row 3: a crash
between fsync and reply leaves a durable write the client believes failed. That
is inherent to a single-round-trip protocol without idempotency tokens, and it
is why the kill-9 harness accepts "the final in-flight op may be present or
absent" as correct.

**What is NOT protected.** The log is a bare RESP stream with no per-record
checksum, so replay detects a torn tail (it does not parse) but not bit rot
mid-file, which would replay as a plausible command. CRC32 per record is the
fix and is listed under "What I'd build next" rather than claimed here.

**Measured consequence.** See the fsync microbenchmark and the three `wal_*`
rows in the README table — that spread is the price of the promise. Note both
were measured inside a Docker VM, where fsync does not reach a physical
platter; treat the ratio as the signal and the absolute µs as an upper bound on
how good the hardware looks, not a disk spec.

---

## DECISION-3 — clock source for TTL + replay determinism

**Context.** CP3 stores expire-at timestamps in ms. `util/clock.h` exposes
wall AND steady time; which anchors TTLs, and how expiry stays deterministic
when the WAL is replayed (CP4), is unresolved.
**Options.** (a) wall clock. (b) steady clock. (c) steady for scheduling,
wall only at persistence boundaries.
**Trade-offs.** Wall matches user intent (`EXPIRE k 10` means 10 real
seconds) and serializes naturally — but an NTP step mass-expires or resurrects
keys. Steady never steps but has an arbitrary epoch that doesn't survive a
process restart, so persisted deadlines need translation. Replay must NOT
re-evaluate expiry against the replaying clock: log an explicit `DEL` at
eviction time (the CP3↔CP4 contract) or justify an alternative here.

**Resolution.** **(a) Wall clock, with replay determinism bought separately.**

Steady time cannot be persisted. Its epoch is arbitrary and dies with the
process, so every deadline in a snapshot would need translating through an
offset captured at save time — and that offset is itself a wall-clock reading,
so the wall clock is in the design either way, just hidden and one indirection
further from the bug. Wall deadlines serialize as themselves and mean the same
thing to a snapshot, a log record, and a human reading `TTL`.

The NTP hazard is accepted and bounded: a backwards step resurrects keys that
should have died (they die on the next access or active cycle once the clock
catches up); a forwards step mass-expires. Neither corrupts data, and hosts
running `ntpd` in slew rather than step mode do not see it at all.

**Determinism is not bought from the clock — it is bought by logging effects.**
Two mechanisms, both required:

1. **Eviction logs an explicit `DEL`.** When a key expires — lazily or in an
   active cycle — the evict hook appends `DEL key`. Replay therefore *replays a
   deletion* instead of *re-deciding* whether the key is dead against a clock
   that has moved.
2. **Relative expirations are rewritten to absolute before they hit the log.**
   `SET k v EX 10` and `EXPIRE k 10` are logged as `SET k v` + `PEXPIREAT k
   <absolute-ms>`. Logging the relative form would re-evaluate "+10s" against
   the recovery clock and silently extend every TTL by the length of the
   downtime. `PEXPIREAT` exists in the command table almost entirely for this;
   real Redis performs the same translation into its AOF.

Together these make recovery a function of the log alone, not of when recovery
happened to run.

**Measured consequence.** `test_ttl.py` covers both expiry paths end-to-end;
`cp3_expiry_test.cpp` drives the deadline logic through `ManualClock`, so the
TTL semantics are tested without a single `sleep`. Active expiry reclaims ≥95%
of 10,000 staggered-TTL keys inside its sampling budget.

---

## DECISION-4 — threading architecture

**Context.** CP5. Must be resolved BEFORE writing shard.cpp, with reference to
the M2 single-thread benchmark profile — not vibes.
**Options.** (a) one reactor + worker pool over a single locked dict.
(b) N shared-nothing shards, each an epoll loop + own dict (SO_REUSEPORT or
hash-routed). (c) stay single-threaded and argue why.
**Trade-offs.** (a) is the smallest diff but the lock is Amdahl's ceiling and
the workers add handoff latency. (b) scales linearly on uniform keyspaces —
until a hot key pins one shard, and multi-key ops now cross shards (MSET —
discussable, not solvable here). (c) is Redis's own answer for years: if the
M2 profile shows the loop is not CPU-bound, threads buy nothing.
Acceptance regardless of choice: 16 clients × 500 INCRs == exactly 8000, TSan
clean, benchmark row per thread count.

**Resolution.** **(a), reshaped: N reactor threads over one locked keyspace.**

Not the literal (a). A single reactor feeding a worker pool hands every command
across a queue and pays a wakeup for it, which puts a scheduler round-trip in
the path of a sub-microsecond `GET`. Instead each thread runs a *complete*
epoll loop and owns the connections it accepted. Everything that dominates the
per-command cost — `recv`, RESP parsing, reply encoding, `send` — runs fully
parallel with no coordination. Only the keyspace mutation takes the lock.

**Why not (b), shared-nothing.** Correctness first: `INCR counter` from 16
connections must total exactly 8,000, and under `SO_REUSEPORT` those
connections land on arbitrary threads. Shared-nothing therefore requires
routing each command to the thread that *owns the key* — which is (a)'s
cross-thread hop reintroduced, with worse tail latency and a hot-key pathology
where one shard pins a core while the rest idle. It is the right answer for a
uniform, high-core-count deployment and the wrong one to reach for first.

**Why one lock and not hash-striped locks.** Striping is maybe sixty lines on
top of what is here (`util/hash.h` already provides `fnv1a64`). I did not add
it because I could not yet point at a measurement saying the single lock was
the ceiling — and "add locks until the graph looks nicer" is how you get a
system nobody can reason about. The measured row below is what that argument
should be made from; the honest read is that the lock does become the limiter,
and striping is the first thing I would do next.

All threads share one listening socket rather than N `SO_REUSEPORT` sockets.
The accept is a race the kernel already arbitrates — losers get `EAGAIN`, which
the accept loop already treats as "queue drained".

**Measured consequence.** Scaling rows (`hermit_t1/t2/t4/t8`) are in the README
table. Correctness: `test_incr_concurrency.py` passes at 1, 2, 4 and 8 threads
(exactly 8,000, no lost updates). ThreadSanitizer reports **zero warnings**
across the concurrency, pipelining, Tier-1 and slow-reader suites at
`--threads=8`, plus a `redis-benchmark` run and a 32-thread write stress.

---

## DECISION-5 — dict: `std::unordered_map` vs hand-rolled table

**Context.** The keyspace container behind core/db.
**Options.** (a) keep `std::unordered_map`. (b) open-addressing table
(robin-hood or linear probing). (c) (b) + incremental rehash à la Redis.
**Trade-offs.** `unordered_map` is correct and free but chases pointers per
bucket and rehashes with a latency spike. A custom table wins cache locality
and controls rehash pauses — and is a CP6-tier project in itself. SPEC's
recommendation: start with (a), profile under redis-benchmark, and let the
flamegraph make the argument either way; the profiling data IS the interview
answer.

**Resolution.** **(a) Keep `std::unordered_map`.**

The profile does not justify replacing it yet. At the throughputs in the README
table the per-command budget is dominated by syscalls and the reply path, not
by bucket traversal — and with `--threads>1` the keyspace lock sits in front of
the container anyway, so a faster dict would be optimizing behind a queue.
Replacing it is a real project with real risk (incremental rehash is where the
bugs live), and I would rather spend that on striped locks, which the
measurement actually points at.

What I would say in an interview is the honest version of this: I know *what*
is wrong with `unordered_map` — node-per-entry, a pointer chase per lookup, and
a full-table rehash that stalls one unlucky request — and I know the fix is
open addressing with incremental rehash. I have not earned the right to claim I
implemented it, and the numbers do not yet ask me to.

**Measured consequence.** Not on the critical path at current throughput; see
the `t1` rows and the note above about the lock ordering ahead of it.

---

## DECISION-6 — DoS-surface numbers

**Context.** Caps the server enforces at the edges. Currently provisional
defaults in `util/config.h`: max frame 64 MiB, max clients 10000, listener
backlog 511.
**Options / Trade-offs.** Frame cap bounds the `*1000000000\r\n` allocation
attack (CP1 enforces it); too low breaks legitimate large values (real Redis:
512 MB proto-max-bulk-len). Client cap bounds fd + buffer memory; interacts
with `ulimit -n`. Per-connection output buffer limits (slow-reader defense)
may deserve a cap + disconnect policy like Redis's client-output-buffer-limit.

**Resolution.** Four caps, each with a stated reason:

| cap | value | why |
|---|---|---|
| max frame / bulk | 64 MiB (`--max-frame-bytes`) | Bounds the `*1000000000\r\n` allocation attack. Enforced in CP1 *before* any allocation sized by attacker input — the declared length is checked against the cap, never trusted into a `reserve()`. Well under Redis's 512 MB because nothing here has a legitimate reason to hold a 512 MB value. |
| max args per command | 1 Mi (compile-time) | Separate from the frame cap on purpose: a 64 MiB frame budget would otherwise permit ~64 M empty arguments, which costs gigabytes in `std::string` headers before a single payload byte arrives. |
| max clients | 10,000 (`--max-clients`) | Bounds fds and per-connection buffers. Over the cap the server replies `-ERR max number of clients reached` and closes, rather than accepting and dying on `EMFILE`. Must stay below `ulimit -n`; the deployment owns that. |
| output buffer / client | 256 MiB | The slow-reader defense. A client that stops reading makes the server buffer on its behalf; unbounded, one such client is an OOM. Over the cap the connection is dropped. Redis calls this `client-output-buffer-limit`. |

The frame and client caps are flags because they are deployment-shaped; the
arg and output-buffer caps are compile-time because no legitimate workload
tunes them and every knob is a support burden.

**Known gap.** There is no per-client rate limit and no `maxmemory` — the
keyspace itself can grow until the OOM killer arrives. `maxmemory` + eviction
is CP6, which is not implemented; the README says so rather than implying a
bound that does not exist.

**Measured consequence.** Oversized-frame and unterminated-inline rejection are
covered in `cp1_resp_parser_test.cpp`; the output-buffer path is exercised by
`test_slow_reader.py`.
