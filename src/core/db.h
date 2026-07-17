#pragma once
// The keyspace. [Claude Code] — implemented at M3 (after the human's M1/M2,
// per SPEC §4 ordering). Interface is fixed now so CP3/CP4 stubs and tests
// can code against it.
//
// DECISION-5 pending: backing store is std::unordered_map until profiling
// says otherwise.

#include <cstddef>
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
  std::optional<std::string> get(const std::string& key) const;
  void set(const std::string& key, std::string value);
  // -- generic --
  bool del(const std::string& key);
  bool exists(const std::string& key) const;
  Type type(const std::string& key) const;
  std::size_t size() const;
  void clear();
  std::vector<std::string> keys_matching_glob(const std::string& pattern) const;
  // -- lists (Tier 2, after M4) --
  // lpush/rpush/lpop/rpop/lrange/llen land here at M4+.

 private:
  std::unordered_map<std::string, Entry> map_;
};

}  // namespace hermit::core
