#pragma once
// The keyspace. [Claude Code] — implemented at M3 (after the human's M1/M2,
// per SPEC §4 ordering). TTL state lives in ExpiryManager (CP3), not here.
//
// DECISION-5 pending: backing store is std::unordered_map until profiling
// says otherwise.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace hermit::core {

class Db {
 public:
  enum class Type { kNone, kString, kList };

  // Tier 2 (lists) exists to force a second type through this variant — the
  // "how would you add a type?" interview follow-up.
  struct Entry {
    std::variant<std::string, std::deque<std::string>> value;
  };

  // -- strings --
  // Precondition for get(): caller has type-checked (GET on a list is a
  // WRONGTYPE error at the dispatcher, not a nullopt here).
  std::optional<std::string> get(const std::string& key) const;
  // Overwrites regardless of previous type (Redis SET semantics).
  void set(const std::string& key, std::string value);
  // -- generic --
  bool del(const std::string& key);
  bool exists(const std::string& key) const;
  Type type(const std::string& key) const;
  std::size_t size() const;
  void clear();
  // Redis-style glob: * ? [a-z] [^a] \escape.
  std::vector<std::string> keys_matching_glob(const std::string& pattern) const;

  // -- lists (Tier 2) --
  // Precondition for all list ops: caller has type-checked; calling them on a
  // key holding a string throws (loudly surfacing a dispatcher bug).
  std::size_t lpush(const std::string& key, std::string value);
  std::size_t rpush(const std::string& key, std::string value);
  // nullopt if the key is absent. Popping the last element deletes the key
  // (Redis removes empty lists).
  std::optional<std::string> lpop(const std::string& key);
  std::optional<std::string> rpop(const std::string& key);
  std::size_t llen(const std::string& key) const;
  // Redis LRANGE index semantics: negative = from tail, stop inclusive,
  // out-of-range clamped.
  std::vector<std::string> lrange(const std::string& key, int64_t start, int64_t stop) const;

  // -- snapshot access [Claude Code, persist/snapshot only] --
  const std::unordered_map<std::string, Entry>& entries() const { return map_; }
  void restore_entry(std::string key, Entry entry);

 private:
  std::unordered_map<std::string, Entry> map_;
};

}  // namespace hermit::core
