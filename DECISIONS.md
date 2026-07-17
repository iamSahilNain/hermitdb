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
**Resolution (human).** _pending_
**Measured consequence.** _pending_

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
**Resolution (human).** _pending — until then `--wal` requires an explicit
`--fsync=...`; the config refuses to default (enforced in util/config.cpp)._
**Measured consequence.** _pending_

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
**Resolution (human).** _pending_
**Measured consequence.** _pending_

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
**Resolution (human).** _pending_
**Measured consequence.** _pending_

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
**Resolution (human).** _pending — scaffold uses `std::unordered_map`._
**Measured consequence.** _pending_

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
**Resolution (human).** _pending — revisit with real numbers before M7._
**Measured consequence.** _pending_
