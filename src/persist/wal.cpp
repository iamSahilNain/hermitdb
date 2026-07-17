#include "persist/wal.h"

namespace hermit::persist {

Wal::Wal(Options opts) : opts_(std::move(opts)) {}

Wal::~Wal() = default;

// ==== CHECKPOINT 4: YOUR CODE ====
// Stubs: every operation reports kNotImplemented so tests/unit/cp4_wal_test.cpp
// and the kill -9 harness fail loudly.

WalStatus Wal::open_for_append() { return WalStatus::kNotImplemented; }

WalStatus Wal::append(const protocol::Command&) { return WalStatus::kNotImplemented; }

WalStatus Wal::tick_fsync(int64_t) { return WalStatus::kNotImplemented; }

WalStatus Wal::replay(const std::function<void(const protocol::Command&)>&) {
  (void)opts_;
  return WalStatus::kNotImplemented;
}

WalStatus Wal::rewrite(const std::function<bool(const std::string&)>&) {
  return WalStatus::kNotImplemented;
}

// ==== END CHECKPOINT 4 ====

}  // namespace hermit::persist
