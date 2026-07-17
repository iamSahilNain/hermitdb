#include "core/expiry.h"

namespace hermit::core {

ExpiryManager::ExpiryManager(Db& db, const Clock& clock) : db_(db), clock_(clock) {}

// ==== CHECKPOINT 3: YOUR CODE ====
// Stubs: no TTLs are ever stored, nothing ever expires, and active_cycle
// reports not_implemented=true so tests/unit/cp3_expiry_test.cpp fails loudly.

void ExpiryManager::set_expiry(const std::string&, int64_t) {}

void ExpiryManager::persist(const std::string&) {}

std::optional<int64_t> ExpiryManager::expiry_at(const std::string&) const { return std::nullopt; }

bool ExpiryManager::check_and_expire(const std::string&) {
  (void)db_;
  (void)clock_;
  return false;
}

ExpiryManager::ActiveCycleStats ExpiryManager::active_cycle(int64_t) { return {}; }

// ==== END CHECKPOINT 3 ====

}  // namespace hermit::core
