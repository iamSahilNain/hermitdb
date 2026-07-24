#include "core/expiry.h"

#include <algorithm>
#include <chrono>

namespace hermit::core {
namespace {

// Redis samples 20 per round and loops while >25% of the sample was expired.
// The threshold is what makes this converge instead of scanning: if a fraction
// p of TTL keys are expired, a sample of 20 finds ~20p of them, so each round
// removes a constant fraction of the dead set — geometric decay, and the loop
// exits as soon as the keyspace is mostly clean.
constexpr std::size_t kSamplePerRound = 20;
constexpr double kContinueThreshold = 0.25;

int64_t micros_now() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

ExpiryManager::ExpiryManager(Db& db, const Clock& clock) : db_(db), clock_(clock) {}

// ==== CHECKPOINT 3: YOUR CODE ====

bool ExpiryManager::untrack(const std::string& key) {
  auto it = index_.find(key);
  if (it == index_.end()) return false;
  const std::size_t slot = it->second;
  const std::size_t last = slots_.size() - 1;
  if (slot != last) {
    slots_[slot] = std::move(slots_[last]);
    index_[slots_[slot].key] = slot;  // the moved key now lives here
  }
  slots_.pop_back();
  index_.erase(it);
  return true;
}

void ExpiryManager::set_expiry(const std::string& key, int64_t at_ms) {
  auto it = index_.find(key);
  if (it != index_.end()) {
    slots_[it->second].at_ms = at_ms;  // re-arm in place
    return;
  }
  slots_.push_back(Slot{key, at_ms});
  index_.emplace(key, slots_.size() - 1);
}

void ExpiryManager::persist(const std::string& key) { untrack(key); }

std::optional<int64_t> ExpiryManager::expiry_at(const std::string& key) const {
  auto it = index_.find(key);
  if (it == index_.end()) return std::nullopt;
  return slots_[it->second].at_ms;
}

bool ExpiryManager::check_and_expire(const std::string& key) {
  auto it = index_.find(key);
  if (it == index_.end()) return false;  // no TTL: immortal, O(1) miss
  if (clock_.wall_ms() < slots_[it->second].at_ms) return false;

  // Untrack before evicting so the hook can never observe a half-dead key —
  // it is allowed to re-enter the dispatcher path (WAL DEL) on our behalf.
  untrack(key);
  if (evict_) evict_(key);
  return true;
}

ExpiryManager::ActiveCycleStats ExpiryManager::active_cycle(int64_t budget_us) {
  ActiveCycleStats stats;
  stats.not_implemented = false;
  if (slots_.empty()) return stats;

  const int64_t deadline = micros_now() + budget_us;
  const int64_t now = clock_.wall_ms();

  do {
    std::size_t sampled = 0;
    std::size_t expired = 0;
    const std::size_t rounds_target = std::min(kSamplePerRound, slots_.size());

    for (std::size_t i = 0; i < rounds_target && !slots_.empty(); ++i) {
      std::uniform_int_distribution<std::size_t> pick(0, slots_.size() - 1);
      const std::size_t slot = pick(rng_);
      ++sampled;
      if (now < slots_[slot].at_ms) continue;
      // Copy: untrack() will move slots_ around underneath this reference.
      const std::string key = slots_[slot].key;
      untrack(key);
      if (evict_) evict_(key);
      ++expired;
    }

    stats.sampled += sampled;
    stats.expired += expired;
    ++stats.rounds;

    if (sampled == 0) break;
    if (static_cast<double>(expired) / static_cast<double>(sampled) <= kContinueThreshold) break;
  } while (micros_now() < deadline && !slots_.empty());

  return stats;
}

// ==== END CHECKPOINT 3 ====

}  // namespace hermit::core
