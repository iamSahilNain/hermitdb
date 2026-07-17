# CP5 — Threading Model

**Files:** `src/core/shard.{h,cpp}` + event-loop integration · **Milestone:** M6
**Decide FIRST:** DECISION-4 — against your own M2 benchmark profile, not
vibes. The shape of everything below depends on it; reshape the ShardedServer
interface freely once decided.
**Test:** `ctest --test-dir build -L cp5 --output-on-failure` (INCR
lost-update detector) + the TSan CI job + benchmark rows at threads=1/2/4/8.

## Contract

Whatever DECISION-4 picks, deliver: correctness under `redis-benchmark -c 50`
with concurrent INCR — 16 clients × 500 INCRs must end at exactly 8000 (the
canonical lost-update detector, already written as an integration test);
ThreadSanitizer clean; a benchmark row per thread count showing where scaling
stops and why. If you pick (c) stay-single-threaded, this checkpoint becomes
the written argument plus the measurements proving the loop isn't CPU-bound —
that is a legitimate completion, not an escape hatch.

## Guiding questions

1. From your M2 profile: what fraction of a request's wall time is epoll/
   syscalls vs parsing vs dict access? What does Amdahl predict from those
   numbers for 2/4/8 threads — before you build anything?
2. For (b) shared-nothing: what routes a connection to a shard —
   SO_REUSEPORT's kernel hash on the 4-tuple, or your own `fnv1a64(key) %
   nshards` at dispatch? What breaks differently in each?
3. What data does each design actually share? Enumerate every shared cache
   line, including "just counters".
4. Where does SHUTDOWN's stop signal have to travel in each design?
5. What happens to DBSIZE, KEYS, FLUSHALL — commands whose answer spans all
   shards?

## Interview questions you must be able to answer (SPEC §5)

- Why is Redis single-threaded and still fast?
- Lock granularity — global mutex vs per-shard vs lock-free.
- Why sharding by `hash(key) % nshards`, and what breaks for multi-key ops
  (MSET across shards — discuss, don't solve).
- False sharing, cache lines.
- Amdahl on your own benchmark numbers.

## Edge cases

- A hot key (every client INCRing one counter) — what does each design
  degrade to?
- Client count < shard count; shard count > core count.
- TTL active-expiry ticks: one global pass or per-shard? Who owns the
  TTL map now?
- WAL: one file with a lock, or one per shard — and what does recovery
  ordering mean across shard logs?
