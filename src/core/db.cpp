#include "core/db.h"

// M0 scaffold stubs. Real implementation lands in M3 (SPEC §4) — deliberately
// AFTER the human's M1/M2 so the core is hand-built first, not retrofitted
// around generated code. CommandDispatcher answers "-ERR ... not implemented"
// until then, so these no-ops are unreachable from the wire.

namespace hermit::core {

std::optional<std::string> Db::get(const std::string&) const { return std::nullopt; }

void Db::set(const std::string&, std::string) {}

bool Db::del(const std::string&) { return false; }

bool Db::exists(const std::string&) const { return false; }

Db::Type Db::type(const std::string&) const { return Type::kNone; }

std::size_t Db::size() const { return map_.size(); }

void Db::clear() { map_.clear(); }

std::vector<std::string> Db::keys_matching_glob(const std::string&) const { return {}; }

}  // namespace hermit::core
