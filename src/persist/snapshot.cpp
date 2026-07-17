#include "persist/snapshot.h"

#include <cstdint>
#include <cstring>
#include <fstream>

// Full-dict snapshot serialization. [Claude Code, M5-scoped work done early]
//
// Format v1 ("HDB1"), native-endian — a single-host recovery format, not an
// interchange format:
//   magic[4] u64 entry_count
//   per entry: u8 type (0=string 1=list)
//              u32 keylen, key bytes
//              u8 has_ttl [i64 expire_at_ms]
//              string: u32 len, bytes | list: u32 count, then u32 len + bytes
//
// SEAM (CP4): this module writes to exactly the path it is given and fsyncs
// nothing. Atomicity — temp file, fsync, rename over the live snapshot,
// directory fsync, WAL truncation ordering — is owned by Wal::rewrite()
// (CHECKPOINT 4). Boot ordering (snapshot first, then WAL tail) is wired in
// main() at M5.

namespace hermit::persist {
namespace {

constexpr char kMagic[4] = {'H', 'D', 'B', '1'};

void put_u8(std::ofstream& f, uint8_t v) { f.write(reinterpret_cast<const char*>(&v), 1); }
void put_u32(std::ofstream& f, uint32_t v) {
  f.write(reinterpret_cast<const char*>(&v), sizeof v);
}
void put_u64(std::ofstream& f, uint64_t v) {
  f.write(reinterpret_cast<const char*>(&v), sizeof v);
}
void put_i64(std::ofstream& f, int64_t v) { f.write(reinterpret_cast<const char*>(&v), sizeof v); }
void put_bytes(std::ofstream& f, const std::string& s) {
  put_u32(f, static_cast<uint32_t>(s.size()));
  f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool get_u8(std::ifstream& f, uint8_t& v) {
  return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), 1));
}
bool get_u32(std::ifstream& f, uint32_t& v) {
  return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof v));
}
bool get_u64(std::ifstream& f, uint64_t& v) {
  return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof v));
}
bool get_i64(std::ifstream& f, int64_t& v) {
  return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof v));
}
// Length-checked against the bytes actually remaining in the file, so a
// corrupt length field cannot trigger a giant allocation.
bool get_bytes(std::ifstream& f, std::string& out, std::streamoff remaining) {
  uint32_t len = 0;
  if (!get_u32(f, len)) return false;
  if (static_cast<std::streamoff>(len) > remaining) return false;
  out.resize(len);
  return static_cast<bool>(f.read(out.data(), static_cast<std::streamsize>(len)));
}

std::streamoff bytes_left(std::ifstream& f, std::streamoff total) {
  return total - static_cast<std::streamoff>(f.tellg());
}

}  // namespace

bool save_snapshot(const core::Db& db, const core::ExpiryManager& expiry, const std::string& path,
                   std::string* err) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (err) *err = "cannot open '" + path + "' for writing";
    return false;
  }
  f.write(kMagic, sizeof kMagic);
  put_u64(f, db.entries().size());

  for (const auto& [key, entry] : db.entries()) {
    const auto* str = std::get_if<std::string>(&entry.value);
    put_u8(f, str ? 0 : 1);
    put_bytes(f, key);
    // The frozen ExpiryManager interface has no TTL iteration; per-key query
    // is O(1) each and keeps this module independent of CP3 internals.
    auto at = expiry.expiry_at(key);
    put_u8(f, at.has_value() ? 1 : 0);
    if (at) put_i64(f, *at);
    if (str) {
      put_bytes(f, *str);
    } else {
      const auto& list = std::get<std::deque<std::string>>(entry.value);
      put_u32(f, static_cast<uint32_t>(list.size()));
      for (const auto& elem : list) put_bytes(f, elem);
    }
  }
  f.flush();
  if (!f) {
    if (err) *err = "write failed for '" + path + "'";
    return false;
  }
  return true;
}

bool load_snapshot(core::Db& db, core::ExpiryManager& expiry, const std::string& path,
                   std::string* err) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (err) *err = "cannot open '" + path + "'";
    return false;
  }
  const std::streamoff total = static_cast<std::streamoff>(f.tellg());
  f.seekg(0);

  char magic[4] = {};
  if (!f.read(magic, sizeof magic) || std::memcmp(magic, kMagic, sizeof kMagic) != 0) {
    if (err) *err = "bad snapshot magic in '" + path + "'";
    return false;
  }
  uint64_t count = 0;
  if (!get_u64(f, count)) {
    if (err) *err = "truncated snapshot header";
    return false;
  }

  for (uint64_t i = 0; i < count; ++i) {
    uint8_t type = 0, has_ttl = 0;
    std::string key;
    if (!get_u8(f, type) || type > 1 || !get_bytes(f, key, bytes_left(f, total)) ||
        !get_u8(f, has_ttl) || has_ttl > 1) {
      if (err) *err = "corrupt snapshot entry " + std::to_string(i);
      return false;
    }
    int64_t at_ms = 0;
    if (has_ttl && !get_i64(f, at_ms)) {
      if (err) *err = "corrupt snapshot entry " + std::to_string(i);
      return false;
    }
    if (type == 0) {
      std::string value;
      if (!get_bytes(f, value, bytes_left(f, total))) {
        if (err) *err = "corrupt snapshot entry " + std::to_string(i);
        return false;
      }
      db.restore_entry(key, core::Db::Entry{std::move(value)});  // key reused below for TTL
    } else {
      uint32_t n = 0;
      if (!get_u32(f, n)) {
        if (err) *err = "corrupt snapshot entry " + std::to_string(i);
        return false;
      }
      std::deque<std::string> list;
      for (uint32_t j = 0; j < n; ++j) {
        std::string elem;
        if (!get_bytes(f, elem, bytes_left(f, total))) {
          if (err) *err = "corrupt snapshot entry " + std::to_string(i);
          return false;
        }
        list.push_back(std::move(elem));
      }
      db.restore_entry(key, core::Db::Entry{std::move(list)});
    }
    if (has_ttl) expiry.set_expiry(key, at_ms);
  }
  return true;
}

}  // namespace hermit::persist
