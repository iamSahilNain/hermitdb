# CP3 — TTL Expiry

**Files:** `src/core/expiry.{h,cpp}` · **Milestone:** M4
**Decide first:** DECISION-3 (clock source + replay determinism).
**Test:** `ctest --test-dir build -L cp3 --output-on-failure`

## Contract

Two cooperating mechanisms, like real Redis. **Lazy:** `check_and_expire(key)`
runs on every key access (the dispatcher calls it before touching `Db`); if
the deadline has passed, evict on the spot via the hook and report "treat as
absent" — O(1). **Active:** `active_cycle(budget_us)` runs from the event-loop
tick (no extra thread): sample N random keys from the TTL-bearing set, evict
the expired; if the expired fraction of the sample beats a threshold, repeat —
never exceeding the time budget. The class owns the key→deadline map (`Db`
stays TTL-oblivious); `set_expiry`/`persist`/`expiry_at` manage it. ALL
eviction goes through the evict hook — at M5 that hook also logs a `DEL` to
the WAL, because expiry must be *deterministic on replay*, never re-evaluated
against a later clock.

## Guiding questions

1. What container supports insert, erase, lookup, AND uniform random sampling,
   all O(1)? (`std::unordered_map` alone does not give you the last one.)
2. Pick sample size N and repeat-threshold: if 25% of sampled keys are
   expired, what does that estimate about the whole TTL set, and with what
   confidence? (This is a CF-style probability argument — write it out.)
3. Where can the active pass and the lazy path race each other, even
   single-threaded? (Think: eviction hook calls back into Db.)
4. Under your DECISION-3 clock, what exactly happens to armed TTLs when NTP
   steps the wall clock backwards 30s? Forwards?
5. Why does the budget need to be time-based rather than count-based?

## Interview questions you must be able to answer (SPEC §5)

- Why not one timer per key?
- Why not a full scan?
- Why sampling converges (the expected-fraction argument).
- Wall clock vs `steady_clock` for TTLs — what breaks on NTP step?
- How does expiry interact with the WAL? (Hint: log a `DEL`.)

## Edge cases

- Re-arming an existing TTL; disarming via `persist`; TTL on a missing key
  (dispatcher's problem, but define the boundary).
- Deadline exactly == now.
- 10k expired keys but a 2ms budget — must make progress across cycles, not
  in one (test).
- Empty TTL set: the cycle must be a near-free no-op (it runs 10×/second
  forever).
- A key deleted normally (DEL) that still has a TTL entry — who cleans up?
