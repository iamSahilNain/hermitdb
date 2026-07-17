#include "core/db.h"

namespace hermit::core {
namespace {

// Matches one non-'*' pattern unit at pat[pi] against c; advances pi past the
// unit. Supports ? , \escape , [set] with ranges and ^negation. An unclosed
// '[' matches a literal '[' (mirrors redis stringmatchlen's forgiveness).
bool match_unit(std::string_view pat, std::size_t& pi, char c) {
  char pc = pat[pi];
  if (pc == '?') {
    ++pi;
    return true;
  }
  if (pc == '\\' && pi + 1 < pat.size()) {
    pi += 2;
    return pat[pi - 1] == c;
  }
  if (pc == '[') {
    std::size_t j = pi + 1;
    bool negate = j < pat.size() && pat[j] == '^';
    if (negate) ++j;
    bool matched = false;
    std::size_t k = j;
    std::size_t close = std::string_view::npos;
    while (k < pat.size()) {
      if (pat[k] == ']' && k > j) {
        close = k;
        break;
      }
      char lo = pat[k];
      if (lo == '\\' && k + 1 < pat.size()) lo = pat[++k];
      if (k + 2 < pat.size() && pat[k + 1] == '-' && pat[k + 2] != ']') {
        char hi = pat[k + 2];
        if (lo <= c && c <= hi) matched = true;
        k += 3;
      } else {
        if (lo == c) matched = true;
        ++k;
      }
    }
    if (close == std::string_view::npos) {
      ++pi;
      return c == '[';
    }
    pi = close + 1;
    return negate ? !matched : matched;
  }
  ++pi;
  return pc == c;
}

// Iterative glob with single-point backtracking (correct for any number of
// stars; no recursion, so hostile patterns can't blow the stack).
bool glob_match(std::string_view pat, std::string_view str) {
  std::size_t pi = 0, si = 0;
  std::size_t star_pi = std::string_view::npos, star_si = 0;
  while (si < str.size()) {
    if (pi < pat.size() && pat[pi] == '*') {
      star_pi = ++pi;
      star_si = si;
      continue;
    }
    std::size_t next_pi = pi;
    if (pi < pat.size() && match_unit(pat, next_pi, str[si])) {
      pi = next_pi;
      ++si;
      continue;
    }
    if (star_pi != std::string_view::npos) {
      pi = star_pi;
      si = ++star_si;
      continue;
    }
    return false;
  }
  while (pi < pat.size() && pat[pi] == '*') ++pi;
  return pi == pat.size();
}

}  // namespace

std::optional<std::string> Db::get(const std::string& key) const {
  auto it = map_.find(key);
  if (it == map_.end()) return std::nullopt;
  if (const auto* s = std::get_if<std::string>(&it->second.value)) return *s;
  return std::nullopt;  // list under a type-checked caller: unreachable
}

void Db::set(const std::string& key, std::string value) {
  map_[key].value = std::move(value);
}

bool Db::del(const std::string& key) { return map_.erase(key) > 0; }

bool Db::exists(const std::string& key) const { return map_.count(key) > 0; }

Db::Type Db::type(const std::string& key) const {
  auto it = map_.find(key);
  if (it == map_.end()) return Type::kNone;
  return std::holds_alternative<std::string>(it->second.value) ? Type::kString : Type::kList;
}

std::size_t Db::size() const { return map_.size(); }

void Db::clear() { map_.clear(); }

std::vector<std::string> Db::keys_matching_glob(const std::string& pattern) const {
  std::vector<std::string> out;
  for (const auto& [key, entry] : map_) {
    (void)entry;
    if (glob_match(pattern, key)) out.push_back(key);
  }
  return out;
}

std::size_t Db::lpush(const std::string& key, std::string value) {
  auto& entry = map_.try_emplace(key, Entry{std::deque<std::string>{}}).first->second;
  auto& list = std::get<std::deque<std::string>>(entry.value);
  list.push_front(std::move(value));
  return list.size();
}

std::size_t Db::rpush(const std::string& key, std::string value) {
  auto& entry = map_.try_emplace(key, Entry{std::deque<std::string>{}}).first->second;
  auto& list = std::get<std::deque<std::string>>(entry.value);
  list.push_back(std::move(value));
  return list.size();
}

std::optional<std::string> Db::lpop(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return std::nullopt;
  auto& list = std::get<std::deque<std::string>>(it->second.value);
  std::string out = std::move(list.front());
  list.pop_front();
  if (list.empty()) map_.erase(it);
  return out;
}

std::optional<std::string> Db::rpop(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return std::nullopt;
  auto& list = std::get<std::deque<std::string>>(it->second.value);
  std::string out = std::move(list.back());
  list.pop_back();
  if (list.empty()) map_.erase(it);
  return out;
}

std::size_t Db::llen(const std::string& key) const {
  auto it = map_.find(key);
  if (it == map_.end()) return 0;
  return std::get<std::deque<std::string>>(it->second.value).size();
}

std::vector<std::string> Db::lrange(const std::string& key, int64_t start, int64_t stop) const {
  auto it = map_.find(key);
  if (it == map_.end()) return {};
  const auto& list = std::get<std::deque<std::string>>(it->second.value);
  const int64_t n = static_cast<int64_t>(list.size());
  if (start < 0) start += n;
  if (stop < 0) stop += n;
  if (start < 0) start = 0;
  if (stop >= n) stop = n - 1;
  if (start > stop || start >= n) return {};
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(stop - start + 1));
  for (int64_t i = start; i <= stop; ++i) out.push_back(list[static_cast<std::size_t>(i)]);
  return out;
}

void Db::restore_entry(std::string key, Entry entry) {
  map_[std::move(key)] = std::move(entry);
}

}  // namespace hermit::core
