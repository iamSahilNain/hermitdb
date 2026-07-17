# HANDOFF — scaffolding session 2 (M3 + Tier 2 + snapshot + bench verify)

Session date: 2026-07-17. Base: M0 (`ef83839`), head: this commit.
All work is `scaffold:`-prefixed; **zero checkpoint files were touched**,
verified mechanically:

```bash
git diff ef83839 HEAD --stat -- src/protocol/resp_parser.{h,cpp} \
  src/net/event_loop.{h,cpp} src/core/expiry.{h,cpp} src/core/shard.{h,cpp} \
  src/persist/wal.{h,cpp} checkpoints/
# (empty output = untouched)
```

Note: no `SPEC_kvstore.md` exists on disk — I worked from the spec as handed
over in-session. Worth committing it to the repo root so it's canonical.

## What was built

- **Db keyspace** (`src/core/db.{h,cpp}`): strings + lists through the Entry
  variant, Redis glob matcher (`* ? [] ^ \`, iterative, no recursion), plus
  two additive snapshot accessors (`entries()`, `restore_entry()`). The M0
  public surface is byte-identical; lists fill the seam the M0 header
  reserved.
- **Tier 1 + Tier 2 commands** (`src/core/commands.cpp`): the full SPEC §3
  set with exact Redis error semantics, as free functions behind a dispatch
  table — the frozen `CommandDispatcher` header is unchanged. Lazy-expiry
  gate (`check_and_expire`) wired at every key access per the CP3 contract.
- **Snapshot serializer** (`src/persist/snapshot.cpp`): HDB1 binary format,
  strings + lists + optional TTL deadline per key; corrupt-input rejection is
  allocation-safe. Writes to exactly the path given, fsyncs nothing — the
  temp/rename/dir-fsync/truncate orchestration is CP4's `Wal::rewrite()` seam
  (documented at the top of the file).
- **Tests**: 3 new required unit suites (label `m3`: db, commands+lists,
  snapshot) driving the dispatcher directly — argv in, RESP wire literals
  out, no parser anywhere. `integration_tier1` relabeled `post-cp2` and
  self-skips (exit 77) with "requires CP2" while the loop is a stub; a
  `post-cp3` unit test pins the snapshot TTL round-trip.
- **CI/Makefile**: required label set is now `scaffold|m3` (TSan job too);
  everything else stays informational.
- **README**: full SPEC §8 draft (command table, architecture, benchmark
  placeholders, decisions link, how-this-was-built).
- **Bench**: `run_bench.sh` verified end-to-end in the dev container
  (redis-server added to the image): 7 hermitdb configs SKIP loudly, real
  Redis baseline + fsync microbench + markdown table all emit.
  `bench/results/FORMAT.md` documents the output format and the virtiofs
  caveat (don't publish fsync numbers from a Mac-hosted container).

## Deliberately NOT built (and why)

| Skipped | Blocked on | Where the seam is |
|---|---|---|
| Serving any traffic | **CP2** (event loop) | `main.cpp` exits 1 citing CP2 |
| RESP parsing | **CP1** | `resp_parser.cpp` stub returns kNotImplemented |
| TTLs actually expiring; TTL/PTTL showing real deadlines | **CP3** | dispatcher calls the stub; `post-cp3` tests pin the contract |
| WAL append site in the dispatcher | **CP4** | deliberately unwired — append-vs-reply ordering is CP4's trade-off |
| Snapshot atomicity + boot recovery wiring | **CP4** / M5 | `Wal::rewrite()` contract; `main.cpp` comment marks the boot hook |
| Any `--threads>1` behavior | **CP5** / DECISION-4 | `ShardedServer` stub refuses |
| fsync default | **DECISION-2** | config refuses `--wal` without `--fsync` |
| Relative-expiry clock choice | **DECISION-3** | single `now_ms()` seam in commands.cpp, `FIXME(DECISION-3)` |

## INTERFACE_PROPOSALS entries (none applied — see INTERFACE_PROPOSALS.md)

- **P1** `ExpiryManager::set_expiry_in(key, ms_from_now)` — moves the
  DECISION-3 clock choice fully inside CP3; today a FIXME seam in
  commands.cpp reads `wall_ms()`.
- **P2** `ExpiryManager::clear()` — FLUSHALL currently disarms TTLs per key
  via an O(n) `persist()` sweep.
- **P3** (weak) TTL iteration for snapshot save — currently per-key
  `expiry_at()` queries; fine if that stays O(1).

## Test-suite state at handoff (15 tests)

| Result | Tests |
|---|---|
| PASS (required, `scaffold\|m3`) | scaffold, m3_db, m3_command, m3_snapshot |
| SKIP (by design) | integration_tier1 — "requires CP2", exit 77 |
| FAIL (by design, informational) | cp1_parser, cp3_expiry, cp4_wal, post_cp3_snapshot_ttl, integration ping / concurrency / slow_reader / ttl / kill9 / incr |

## Verify this state yourself

```bash
cd ~/Documents/hermitdb
make image && make configure && make build     # or reuse the existing build/
make test                                       # required set: must be green
make test-all                                   # full picture incl. designed failures
git log --oneline                               # scaffold:-only history
git diff ef83839 HEAD --stat -- src/protocol/resp_parser.{h,cpp} \
  src/net/event_loop.{h,cpp} src/core/expiry.{h,cpp} src/core/shard.{h,cpp} \
  src/persist/wal.{h,cpp} checkpoints/          # empty = checkpoints untouched
# bench harness smoke (real-Redis baseline only):
make shell
N_REQUESTS=20000 ./bench/run_bench.sh ./build/hermitdb
```

## Your critical path from here (unchanged from M0)

CP1 (`ctest -L cp1`) → CP2 (`make run` + `make cli` → PING) — at which point
`integration_tier1` stops skipping and the whole Tier 1/2 surface lights up
end-to-end — then CP3 → CP4 → DECISION-4/CP5 → benchmarks on real hardware.
