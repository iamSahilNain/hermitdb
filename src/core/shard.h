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
  const Config& cfg_;
  // ==== END CHECKPOINT 5 ====
};

}  // namespace hermit::core
