# Interface Proposals

Per the rules of engagement, interfaces frozen at M0 are not changed by
scaffolding work. Where the frozen surface forced a workaround, the proposal
lives here for the human to accept or reject (typically while implementing the
affected checkpoint). Additive extensions that did NOT alter any existing
signature are listed at the bottom for transparency.

---

## P1 — `ExpiryManager::set_expiry_in(key, int64_t ms_from_now)`

**Problem.** `set_expiry(key, at_ms)` takes an absolute deadline, so every
caller converting a relative expiry (`SET ... EX 10`, `EXPIRE k 10`) must ask
a clock for "now" — which forces the DECISION-3 clock choice OUTSIDE the
checkpoint that owns it. Current workaround: a single `now_ms()` seam in
`src/core/commands.cpp` marked `FIXME(DECISION-3)`, currently reading
`wall_ms()`.

**Proposal.** Add a relative-form entry point so the conversion — and the
clock it reads — lives entirely inside CP3. The absolute form can remain for
snapshot restore.

**If rejected.** Keep the seam; when resolving DECISION-3, update `now_ms()`
and the CP3 internals together, and document the pairing in DECISIONS.md.

---

## P2 — `ExpiryManager::clear()`

**Problem.** FLUSHALL must drop all TTLs, but the frozen interface only
disarms per key. Current workaround in `cmd_flushall`: enumerate every key via
`keys_matching_glob("*")` and call `persist(key)` — an extra O(n) pass and an
allocation of all key names, on a command whose whole point is bulk teardown.

**Proposal.** `void clear();` — trivial for any CP3 container choice.

**If rejected.** The workaround is correct, just wasteful; fine for a
non-hot-path command.

---

## P3 — TTL iteration for snapshotting

**Problem.** `save_snapshot` needs each key's deadline; the frozen interface
exposes only per-key `expiry_at(key)`. Current approach: query it for every
dict entry during save — O(1) each, O(n) total, perfectly acceptable — but it
assumes `expiry_at` stays O(1) under whatever container CP3 picks for random
sampling.

**Proposal (weak).** `void for_each_ttl(fn(key, at_ms))` if CP3's container
makes iteration natural. Not urgent; adopt only if P1/P2 are being done anyway.

---

## Additive extensions made (no frozen signature altered)

- `Db::lpush/rpush/lpop/rpop/llen/lrange` — filled the seam the M0 header
  explicitly reserved for Tier 2 ("lists land here").
- `Db::entries()` / `Db::restore_entry()` — read/rebuild access for
  `persist/snapshot` only; documented as such in the header.
