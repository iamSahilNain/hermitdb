# SPEC — `hermitdb`: RESP-Compatible In-Memory Key-Value Store in C++

**Handoff document for Claude Code.** Read this entire file before writing any code.

This project uses a **learning-checkpoint strategy**: Claude Code builds the scaffolding,
build system, tests, and tedious plumbing; the human (Sahil) personally implements every
load-bearing component that interviews probe. The division is non-negotiable and marked
explicitly below. A checkpoint left as a stub is a *success state*, not an incomplete task.

---

## 0. Rules of Engagement for Claude Code

1. **NEVER implement anything inside a `LEARNING CHECKPOINT` region.** Create the stub,
   the interface, the failing tests, and the doc comments — then stop. If a checkpoint
   stub blocks another component, code against the interface; the build must compile
   with all stubs in place (stubs return `NOT_IMPL` errors at runtime).
2. **NEVER make a decision listed in §6 (Decision Log).** Where a decision is pending,
   implement behind an interface so either branch plugs in, and add a `// DECISION-N pending`
   comment. If truly blocked, ask — do not pick.
3. Every checkpoint ships with: (a) a stub file with `// ==== CHECKPOINT N: YOUR CODE ====`
   markers, (b) a doc block stating the contract, (c) unit tests that FAIL until the human
   implements it, (d) a `checkpoints/CP<N>.md` study sheet (see §5 template).
4. Write idiomatic **C++17**, no external frameworks for core logic. Allowed deps:
   Catch2 or GoogleTest (tests only). No Boost, no libevent/libuv (defeats the purpose),
   no third-party RESP or hash-table libraries.
5. Style: RAII everywhere, no raw `new`/`delete`, `-Wall -Wextra -Werror`, clang-format
   config committed. Comments explain *why*, not *what*.
6. Keep commits small and message-honest: scaffolding commits say `scaffold:`, so the git
   history transparently shows what was generated vs. hand-written (`cp<N>:` prefix reserved
   for the human's checkpoint commits).
7. At the end (Milestone M7), run the **adversarial review** protocol in §9.

---

## 1. Goal and Non-Goals

**Goal.** A single-binary TCP server speaking the RESP2 protocol, compatible with the
official `redis-cli` and `redis-benchmark` tools, supporting the command set in §3, with
TTL expiry, write-ahead-log persistence + recovery, snapshotting, a configurable threading
model, and a benchmark suite comparing against real Redis.

**Non-goals (do NOT build):** cluster mode, replication, pub/sub, Lua scripting, RESP3,
ACLs/auth, keyspace notifications, data types beyond strings + lists. Scope discipline is
a feature.

**Definition of done:**
```
$ docker run -p 6380:6380 <img>            # boots
$ redis-cli -p 6380 SET k v EX 10          # official client works
$ redis-benchmark -p 6380 -t set,get -n 1000000 -c 50   # produces numbers
$ kill -9 server && restart                # recovers state from WAL+snapshot
$ ctest                                    # all tests green
```

---

## 2. Repository Layout

```
hermitdb/
├── CMakeLists.txt
├── Dockerfile                  # multi-stage, tiny runtime image
├── .github/workflows/ci.yml    # build + ctest on push
├── .clang-format  .gitignore
├── README.md                   # arch diagram, quickstart, benchmark table (placeholders)
├── DECISIONS.md                # §6 — human fills the Resolution fields
├── checkpoints/CP1.md … CP6.md # study sheets
├── src/
│   ├── main.cpp                # arg parsing, config, wiring        [Claude Code]
│   ├── net/
│   │   ├── event_loop.{h,cpp}  # CHECKPOINT 2
│   │   ├── listener.{h,cpp}    # socket(), bind(), listen(), nonblocking accept [Claude Code]
│   │   └── connection.{h,cpp}  # per-conn buffers, lifecycle        [Claude Code]
│   ├── protocol/
│   │   ├── resp_parser.{h,cpp} # CHECKPOINT 1
│   │   └── resp_writer.{h,cpp} # serialize replies                  [Claude Code]
│   ├── core/
│   │   ├── db.{h,cpp}          # dict, entry struct, string+list ops [Claude Code]
│   │   ├── expiry.{h,cpp}      # CHECKPOINT 3
│   │   ├── commands.{h,cpp}    # dispatch table, arg validation     [Claude Code]
│   │   └── shard.{h,cpp}       # CHECKPOINT 5 (threading model)
│   ├── persist/
│   │   ├── wal.{h,cpp}         # CHECKPOINT 4
│   │   └── snapshot.{h,cpp}    # serialize/load full dict           [Claude Code]
│   └── util/                   # logging, config, hashing, time     [Claude Code]
├── tests/                      # unit + integration                 [Claude Code]
│   └── integration/            # spawns server, drives real sockets
└── bench/
    ├── run_bench.sh            # redis-benchmark matrix, vs real Redis [Claude Code]
    └── results/                # raw outputs land here
```

---

## 3. Command Set (all [Claude Code] in `commands.cpp`, calling into core/)

**Tier 1 (must):** `PING ECHO SET GET DEL EXISTS EXPIRE PEXPIRE TTL PTTL PERSIST INCR
DECR INCRBY TYPE FLUSHALL DBSIZE KEYS <glob> CONFIG GET (stub ok) COMMAND (stub ok —
redis-cli sends it on connect) SHUTDOWN`

`SET` options: `EX PX NX XX` (needed for redis-benchmark and TTL tests).

**Tier 2 (after M4):** `LPUSH RPUSH LPOP RPOP LRANGE LLEN` — exists to force a second
value type through the entry struct (interview follow-up: "how would you add a type?").

**Error semantics** mirror Redis: `-ERR wrong number of arguments for 'get' command`,
`-WRONGTYPE ...`, integer replies `:N`, bulk `$`, arrays `*`, null bulk `$-1`.

---

## 4. Milestones

| M | Deliverable | Owner split |
|---|-------------|-------------|
| M0 | Repo, CMake, CI, logging, config, all stubs compiling | Claude Code |
| M1 | RESP parser (CP1) passes unit suite; echo server via blocking I/O harness | **Human** |
| M2 | epoll event loop (CP2); `redis-cli PING` works end-to-end, 1 thread | **Human** |
| M3 | db + commands Tier 1 wired; `redis-benchmark -t set,get` runs | Claude Code |
| M4 | TTL expiry (CP3); `SET k v EX 1` + sleep tests pass | **Human** |
| M5 | WAL (CP4) + snapshot; kill -9 recovery test passes | **Human** (WAL) / CC (snapshot) |
| M6 | Threading model (CP5); benchmark matrix, README table filled | **Human** |
| M7 | Adversarial review (§9), Docker Hub push, optional CP6 | joint |

Order matters: the human's M1/M2 come *before* the bulk scaffolding of M3 so the core is
hand-built first, not retrofitted around generated code.

---

## 5. LEARNING CHECKPOINTS — the human implements these

Each `checkpoints/CP<N>.md` study sheet Claude Code writes must contain: the contract
restated, 3–5 guiding questions (no answers), the exact interview questions listed below,
edge-case list, and the test command. **No reference implementations, no pseudocode.**

### CP1 — Incremental RESP2 Parser (`protocol/resp_parser.{h,cpp}`)

**Contract:** `class RespParser { ParseResult feed(std::string_view bytes); }` — consumes
arbitrary byte chunks (TCP gives no message boundaries), yields zero or more complete
commands (`std::vector<std::string>`), retains partial state across calls. Must handle:
a command split across 10 one-byte reads; two commands arriving in one read; inline
commands (`PING\r\n` without array framing — redis-cli uses these); protocol errors →
recoverable error result, connection closes.

**Why it's the human's:** "parse a framed protocol off a stream" is a canonical
GS/DE Shaw-tier question; partial-read handling is the entire difficulty of network code.

**Interview mapping:** How do you know a message is complete? Why can't you `recv()` a
"whole command"? State machine vs recursive descent — why here? What's the memory blow-up
attack (`*1000000000\r\n`) and your defense (cap + error)?

**Tests (Claude Code writes, must fail on stub):** byte-at-a-time fuzz of the full command
corpus, pipelined batch, malformed length, oversized frame rejection.

### CP2 — epoll Event Loop (`net/event_loop.{h,cpp}`)

**Contract:** single-threaded reactor. `run()` loops on `epoll_wait`; dispatches accept →
new `Connection`, readable → drain `recv` into conn buffer → feed parser → execute →
queue reply, writable → flush pending writes; handles EAGAIN, short writes (kernel buffer
full ⇒ register EPOLLOUT, deregister when drained), peer close, RST. Listener FD and
connection FDs all nonblocking (Claude Code provides `set_nonblocking`, listener setup).

**Decision required first:** DECISION-1 (LT vs ET).

**Interview mapping:** epoll vs select/poll — what's actually O(1)? LT vs ET semantics and
the ET starvation bug. Why nonblocking sockets are mandatory with ET. What happens when
`send()` returns a short write? Thundering herd. C10K in one paragraph.

**Tests:** integration — 200 concurrent clients pipelining; a "slow reader" client that
never drains (server must not block others); abrupt disconnect mid-command.

### CP3 — TTL Expiry (`core/expiry.{h,cpp}`)

**Contract:** two cooperating mechanisms, like real Redis: (a) **lazy** — every key access
checks expiry and deletes on the spot; (b) **active** — a periodic pass (invoked from the
event-loop tick, no extra thread in single-threaded mode) that samples N random keys from
the TTL-bearing set, evicts the expired, and repeats within a time budget if the expired
fraction exceeds a threshold. Expiry timestamps in ms, monotonic-clock question is yours
to confront (DECISION-3).

**Interview mapping:** Why not one timer per key? Why not a full scan? Why sampling
converges (expected-fraction argument — this is a probability question, play to CP
strength). Wall clock vs `steady_clock` for TTLs — what breaks on NTP step? How does
expiry interact with the WAL (hint: expiry must be *deterministic on replay* — log a `DEL`).

**Tests:** 100k keys, staggered TTLs, assert memory reclaimed within budget; replay
determinism test tying into CP4.

### CP4 — Write-Ahead Log (`persist/wal.{h,cpp}`)

**Contract:** append every *mutating* command (post-validation, pre-reply... or post-reply?
— that ordering trade-off is yours to reason about and document) in RESP encoding to an
append-only file; `fsync` per DECISION-2 policy; on boot, load latest snapshot then replay
WAL tail; expose `rewrite()` = write fresh snapshot + truncate WAL (Claude Code's snapshot
module does serialization; you own the *ordering and atomicity* — temp file + `rename()`,
fsync the directory, the crash-window analysis).

**Interview mapping:** What does `write()` returning actually guarantee? Page cache vs
disk; what `fsync` costs (measure it — it goes in the benchmark table). Crash between
WAL append and reply — is the client's ACKed write durable under each fsync policy?
Why `rename()` is the atomic primitive. Group commit — how would you batch fsyncs?

**Tests:** kill -9 harness (Claude Code writes): random workload → SIGKILL at random
offsets → restart → state equals a shadow model (for `always` policy) / prefix-consistent
(for `everysec`). Torn-final-record tolerance.

### CP5 — Threading Model (`core/shard.{h,cpp}` + event-loop integration)

**Contract:** depends entirely on DECISION-4, which you must resolve *before* coding.
Whichever you choose, deliver: correctness under `redis-benchmark -c 50` with mixed
INCR (the canonical lost-update detector: N clients × M INCRs must end at exactly N·M),
and a benchmark row per thread count (1/2/4/8).

**Interview mapping:** Why is Redis single-threaded and still fast? Lock granularity —
global mutex vs per-shard vs lock-free; why sharding by `hash(key) % nshards` and what
breaks for multi-key ops (MSET across shards — you don't have to solve it, you have to be
able to *discuss* it). False sharing, cache lines. Amdahl on your own benchmark numbers.

**Tests:** ThreadSanitizer job in CI (Claude Code adds `-fsanitize=thread` build); the
INCR correctness test; throughput regression guard.

### CP6 (optional, only if M0–M6 done) — LRU Eviction

`maxmemory` + O(1) LRU (intrusive doubly-linked list + dict), or Redis-style sampled
approximation — your pick, argue it in DECISIONS.md. Interview mapping: the classic
LRU-design question, but you'll have implemented it under real memory accounting.

---

## 6. DECISIONS.md — the human resolves these; Claude Code only formats the file

Template per entry: Context / Options / Trade-offs / **Resolution (human-written)** /
Measured consequence (filled after benchmarks).

- **DECISION-1:** epoll level-triggered vs edge-triggered.
- **DECISION-2:** fsync policy default — `always` vs `everysec` vs `no` (+ make it a
  config flag; benchmark all three — this row is the money row of the README table).
- **DECISION-3:** clock source for TTL (wall vs steady) and replay determinism strategy.
- **DECISION-4:** threading architecture — (a) single reactor + worker pool sharing one
  locked dict, (b) N shared-nothing shards each with own epoll loop + dict (SO_REUSEPORT
  or hash-routed), (c) stay single-threaded and argue why. Pick with reference to your
  own M2 benchmark profile, not vibes.
- **DECISION-5:** dict — `std::unordered_map` vs hand-rolled open-addressing table.
  (Recommended: start with `unordered_map`, profile, and *then* decide if a custom table
  is a justified CP6-tier add; either way the profiling data is the interview answer.)
- **DECISION-6:** max frame size / max clients / buffer sizing — the DoS-surface numbers.

---

## 7. Benchmark Protocol (`bench/run_bench.sh`, Claude Code)

Matrix: {hermitdb 1/2/4/8 threads, real redis-server} × {SET, GET, INCR, mixed} ×
{fsync always, everysec, no} — `redis-benchmark -n 1000000 -c 50 -P {1,16}` (pipelining
on/off). Emit a markdown table + p50/p99 latency into `bench/results/` and a README
section. Also a one-off `fsync` microbenchmark (µs per fsync on the target disk) —
that number anchors the CP4 interview story.

Honesty rule: numbers in the README must be reproducible by `./bench/run_bench.sh` on
stated hardware. No cherry-picking; if real Redis wins (it will on some rows), the table
says so and DECISIONS.md explains why.

---

## 8. README (Claude Code drafts, human finalizes)

Quickstart (docker one-liner), 30-sec asciinema/GIF placeholder, architecture diagram
(event loop → parser → dispatch → shards → WAL, ASCII or mermaid), benchmark table,
"Design decisions" section that *links to* DECISIONS.md, "What I'd build next" (replication,
RESP3, io_uring — one paragraph, shows awareness of the frontier). Explicitly credit the
scaffolding split: a short "How this was built" note stating which parts were hand-written
— this preempts the "did AI write this?" question by answering it with the git history.

---

## 9. Adversarial Review (M7 — run this as the final session)

Claude Code interrogates the human, viva-style, over the checkpoint code: minimum 15
questions drawn from the interview mappings above plus code-specific probes ("this branch
in your parser — what input reaches it?"). Protocol: any answer that's wrong or hollow →
the human rewrites/annotates that region *personally* and the question re-queues. Produce
`checkpoints/VIVA.md` logging every question, verdict (SOLID / SHAKY / REWRITTEN), and
follow-ups. The project is not done until zero SHAKY remain. Failed items also get logged
to the Code Forensics tracker under a `fundamentals` tag.

---

## 10. Resume Bullet Targets (write nothing until numbers exist)

Shape to aim for, pending real measurements:

- Redis-compatible in-memory KV store in C++17 (epoll reactor, RESP2); interoperable with
  official redis-cli/redis-benchmark; X ops/sec (Y% of Redis) at p99 Z ms.
- Write-ahead-log persistence with configurable fsync policy and crash recovery verified
  by a kill -9 fuzzing harness; N-shard threading model scaling to K× single-thread throughput.
