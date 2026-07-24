#pragma once
#include "util/config.h"

namespace hermit::core {

enum class ShardStatus {
  kStopped,
  kFatalError,
  kNotImplemented,  // stub — CHECKPOINT 5 pending
};

// ============================= CHECKPOINT 5 =================================
// Threading model. Study sheet: checkpoints/CP5.md
//
// DECISION-4 pending — and this checkpoint's shape depends ENTIRELY on it:
//   (a) single reactor + worker pool over one locked dict,
//   (b) N shared-nothing shards, each with its own epoll loop + dict
//       (SO_REUSEPORT or hash-routed),
//   (c) stay single-threaded and argue why (then this class stays a refusal).
// Resolve DECISION-4 in DECISIONS.md — against your own M2 benchmark profile,
// not vibes — BEFORE writing any code here. The interface below is the
// minimal wiring surface main() needs; reshape it freely once the decision
// is made (util/hash.h provides fnv1a64 for hash routing if you go (b)).
//
// Non-negotiable acceptance (whatever you pick):
//   N clients x M INCRs each == exactly N*M — no lost updates
//   (tests/integration/test_incr_concurrency.py), clean TSan, and a
//   benchmark row per thread count {1,2,4,8}.
// ============================================================================
class ShardedServer {
 public:
  explicit ShardedServer(const Config& cfg);

  // Blocks until shutdown (like EventLoop::run, but owning nthreads threads).
  ShardStatus start_and_run();

 private:
  // ==== CHECKPOINT 5: YOUR CODE ====
  // DECISION-4 resolved: N REACTOR THREADS OVER ONE LOCKED KEYSPACE — option
  // (a), reshaped. The SPEC's (a) puts one reactor in front of a worker pool;
  // that hands every command across a queue and pays a wakeup for it. Here
  // each thread owns a full epoll loop and the connections it accepted, so
  // recv, RESP parsing, reply encoding and send — the majority of the work —
  // run fully parallel, and only the keyspace mutation itself takes the lock.
  //
  // Why not (b), shared-nothing shards: correctness. `INCR counter` from 16
  // connections must total exactly N*M, and under SO_REUSEPORT those
  // connections land on arbitrary threads. Shared-nothing would need the
  // command routed to the shard OWNING the key — a cross-thread hop per
  // command, which is (a)'s queue cost reintroduced with worse tail latency.
  // Hash-striped locks are the honest next step and are cheap to add; the
  // single lock is here because the benchmark, not a hunch, should justify
  // the complexity. See the measured Amdahl row in DECISIONS.md.
  //
  // All threads share ONE listening socket rather than SO_REUSEPORT sockets.
  // The accept is a race the kernel already arbitrates: losers get EAGAIN,
  // which the accept loop already handles as "queue drained".
  const Config& cfg_;
  // ==== END CHECKPOINT 5 ====
};

}  // namespace hermit::core
