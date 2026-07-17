# CP6 — LRU Eviction (OPTIONAL — only after M0–M6 are done)

**Files:** your choice (likely `src/core/` additions + db integration)
**Decide:** exact vs sampled LRU — argue the pick in DECISIONS.md (add a
DECISION-7 entry).

## Contract

`--maxmemory=N` bounds keyspace memory; at the bound, writes evict per LRU.
Two respectable designs — pick one and defend it:

- **Exact O(1) LRU:** intrusive doubly-linked list threaded through dict
  entries; every access splices to head; evict from tail. The classic
  interview data structure, but every GET now writes.
- **Sampled approximation (Redis's actual choice):** per-entry access
  clock; evict the oldest of K random samples. No list maintenance; eviction
  quality is probabilistic.

Real memory accounting is the hidden half: decide what "memory used" means
(key + value bytes? entry overhead? allocator slack?) and be able to defend
the boundary.

## Guiding questions

1. Why does textbook-exact LRU hurt a read-heavy workload in a way the
   sampled version doesn't?
2. What's the interaction with TTL expiry — is an expired-but-unswept key
   eviction-priority gold?
3. How do you unit-test an approximation?

## Interview mapping

The classic LRU-design question ("design an LRU cache, O(1) get/put") — but
you'll have implemented it under real memory accounting, which is the
follow-up interviewers actually care about.

## Test

Claude Code writes the acceptance tests once you commit to a design — ask.
Minimum: fill past maxmemory, assert bound respected + eviction order sane;
access-pattern test distinguishing LRU from FIFO.
