#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/db.h"
#include "util/clock.h"

namespace hermit::core {

// ============================= CHECKPOINT 3 =================================
// TTL expiry — two cooperating mechanisms, like real Redis.
// Study sheet: checkpoints/CP3.md
// DECISION-3 pending: wall vs steady clock (the Clock interface exposes both;
// which one anchors TTLs — and how replay stays deterministic — is yours).
//
// CONTRACT
//  - This class owns the key -> expire-at-ms map. Db stays TTL-oblivious.
//  - set_expiry(key, at_ms): arm/overwrite a TTL. persist(key): disarm.
//    expiry_at(key): current deadline, nullopt if none.
//  - LAZY: check_and_expire(key) is called on EVERY key access (dispatcher
//    does this before touching Db). If the key's deadline has passed: evict
//    it (via the hook) and return true — the caller then treats the key as
//    absent. Must be O(1).
//  - ACTIVE: active_cycle(budget_us) is called from the event-loop tick, no
//    extra thread. Sample N random TTL-bearing keys, evict the expired;
//    if the expired fraction of the sample exceeds a threshold, repeat —
//    but never past budget_us. Returns stats (with not_implemented=false!).
//  - ALL eviction goes through the evict hook: the hook deletes from Db and,
//    once M5 lands, logs a DEL to the WAL — expiry must be deterministic on
//    replay, never re-evaluated against a later clock.
// ============================================================================
class ExpiryManager {
 public:
  struct ActiveCycleStats {
    std::size_t sampled = 0;
    std::size_t expired = 0;
    std::size_t rounds = 0;
    bool not_implemented = true;  // stub marker; your implementation sets false
  };

  using EvictFn = std::function<void(const std::string& key)>;

  ExpiryManager(Db& db, const Clock& clock);

  void set_evict_hook(EvictFn fn) { evict_ = std::move(fn); }

  void set_expiry(const std::string& key, int64_t at_ms);
  void persist(const std::string& key);
  std::optional<int64_t> expiry_at(const std::string& key) const;

  bool check_and_expire(const std::string& key);
  ActiveCycleStats active_cycle(int64_t budget_us);

 private:
  // ==== CHECKPOINT 3: YOUR CODE ====
  // Your TTL bookkeeping lives here. Think about what "sample N random keys"
  // requires of the container you choose.
  Db& db_;
  const Clock& clock_;
  EvictFn evict_;
  // ==== END CHECKPOINT 3 ====
};

}  // namespace hermit::core
