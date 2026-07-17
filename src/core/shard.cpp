#include "core/shard.h"

#include "util/log.h"

namespace hermit::core {

ShardedServer::ShardedServer(const Config& cfg) : cfg_(cfg) {}

ShardStatus ShardedServer::start_and_run() {
  // ==== CHECKPOINT 5: YOUR CODE ====
  // DECISION-4 pending. Stub refuses to pretend it can run threads.
  logging::error("ShardedServer is a stub — resolve DECISION-4, then implement CHECKPOINT 5");
  (void)cfg_;
  return ShardStatus::kNotImplemented;
  // ==== END CHECKPOINT 5 ====
}

}  // namespace hermit::core
