#pragma once
// FNV-1a: tiny, decent distribution, and trivially explainable in an
// interview. Used for shard routing (CP5). The dict itself hashes via
// std::unordered_map for now — DECISION-5 pending (profile first, then
// decide whether a hand-rolled open-addressing table is justified).

#include <cstdint>
#include <string_view>

namespace hermit {

constexpr uint64_t fnv1a64(std::string_view s) {
  uint64_t h = 1469598103934665603ULL;  // FNV offset basis
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;  // FNV prime
  }
  return h;
}

}  // namespace hermit
