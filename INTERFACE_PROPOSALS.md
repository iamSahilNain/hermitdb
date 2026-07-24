# Interface Proposals

> **Disposition after the CP1–CP5 completion session (2026-07-24):** all three
> proposals were **left unadopted**, and every frozen signature survived the
> checkpoints unchanged. Verdicts are recorded under each entry. The short
> version: P1's problem turned out to be real but relocated rather than
> removed, and P2/P3 were performance nits on cold paths that did not earn an
> interface change.

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

**Verdict: REJECTED — kept the seam.** DECISION-3 resolved to the wall clock,
so `now_ms()` in commands.cpp stayed as written and the pairing is documented.
More interestingly, the proposal would not have solved the real problem. The
thing that actually bites is not *which* clock reads "now", it is that a
relative expiry must never be re-evaluated at replay time — and that is fixed
in CP4 (rewriting `EX`/`EXPIRE` into absolute `PEXPIREAT` before logging), not
by moving the conversion inside CP3. `set_expiry_in` would have hidden the
clock read without making replay any more deterministic.

---

## P2 — `ExpiryManager::clear()`

**Problem.** FLUSHALL must drop all TTLs, but the frozen interface only
disarms per key. Current workaround in `cmd_flushall`: enumerate every key via
`keys_matching_glob("*")` and call `persist(key)` — an extra O(n) pass and an
allocation of all key names, on a command whose whole point is bulk teardown.

**Proposal.** `void clear();` — trivial for any CP3 container choice.

**If rejected.** The workaround is correct, just wasteful; fine for a
non-hot-path command.

**Verdict: REJECTED — workaround kept.** FLUSHALL is not on any hot path, and
CP3's container (a dense slot vector + index map) would make `clear()` a
two-line `O(1)` truncation — which is precisely why it can wait until something
actually needs it. Adding a method to the frozen surface to save an allocation
on a command nobody issues in a loop is the wrong trade.

---

## P3 — TTL iteration for snapshotting

**Problem.** `save_snapshot` needs each key's deadline; the frozen interface
exposes only per-key `expiry_at(key)`. Current approach: query it for every
dict entry during save — O(1) each, O(n) total, perfectly acceptable — but it
assumes `expiry_at` stays O(1) under whatever container CP3 picks for random
sampling.

**Proposal (weak).** `void for_each_ttl(fn(key, at_ms))` if CP3's container
makes iteration natural. Not urgent; adopt only if P1/P2 are being done anyway.

**Verdict: REJECTED — assumption held.** CP3 backs TTLs with an
`unordered_map<string, size_t>` index into a dense vector, so `expiry_at()` is
`O(1)` exactly as the current snapshot path assumes. The proposal was
conditional on that assumption breaking; it didn't.

---

## Additive extensions made (no frozen signature altered)

- `Db::lpush/rpush/lpop/rpop/llen/lrange` — filled the seam the M0 header
  explicitly reserved for Tier 2 ("lists land here").
- `Db::entries()` / `Db::restore_entry()` — read/rebuild access for
  `persist/snapshot` only; documented as such in the header.
