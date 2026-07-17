// CHECKPOINT 3 acceptance suite. FAILS until core/expiry.cpp is implemented.
// Uses ManualClock — no sleeps, no flakiness; time moves when the test says.
//   ctest --test-dir build -L cp3 --output-on-failure

#include "core/expiry.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "core/db.h"
#include "util/clock.h"

using namespace hermit;
using core::Db;
using core::ExpiryManager;

namespace {

struct Fixture {
  Db db;
  ManualClock clock;
  ExpiryManager expiry{db, clock};
  std::set<std::string> evicted;

  Fixture() {
    expiry.set_evict_hook([this](const std::string& k) {
      evicted.insert(k);
      db.del(k);
    });
  }
};

}  // namespace

TEST_CASE("cp3: lazy — key expires exactly at its deadline") {
  Fixture f;
  f.clock.advance(1000);
  f.expiry.set_expiry("k", 5000);

  REQUIRE(f.expiry.expiry_at("k").has_value());
  CHECK(*f.expiry.expiry_at("k") == 5000);

  f.clock.advance(3000);  // t=4000, before deadline
  CHECK_FALSE(f.expiry.check_and_expire("k"));
  CHECK(f.evicted.empty());

  f.clock.advance(1500);  // t=5500, past deadline
  CHECK(f.expiry.check_and_expire("k"));
  CHECK(f.evicted.count("k") == 1);
  // TTL bookkeeping must be gone too.
  CHECK_FALSE(f.expiry.expiry_at("k").has_value());
}

TEST_CASE("cp3: keys without TTL never expire") {
  Fixture f;
  f.clock.advance(1u << 30);
  CHECK_FALSE(f.expiry.check_and_expire("immortal"));
  CHECK(f.evicted.empty());
}

TEST_CASE("cp3: persist disarms a TTL") {
  Fixture f;
  f.expiry.set_expiry("k", 100);
  f.expiry.persist("k");
  CHECK_FALSE(f.expiry.expiry_at("k").has_value());
  f.clock.advance(10000);
  CHECK_FALSE(f.expiry.check_and_expire("k"));
  CHECK(f.evicted.empty());
}

TEST_CASE("cp3: re-arming overwrites the previous deadline") {
  Fixture f;
  f.expiry.set_expiry("k", 100);
  f.expiry.set_expiry("k", 999999);
  f.clock.advance(5000);
  CHECK_FALSE(f.expiry.check_and_expire("k"));
}

TEST_CASE("cp3: active cycle reclaims expired keys without a full scan budget") {
  Fixture f;
  constexpr int kKeys = 10000;
  for (int i = 0; i < kKeys; ++i) {
    // Staggered deadlines 1..5000ms — all in the past once we advance.
    f.expiry.set_expiry("key:" + std::to_string(i), 1 + (i % 5000));
  }
  f.clock.advance(10000);

  // The sampling design must converge: repeated budgeted cycles reclaim
  // (nearly) everything. 200 cycles is generous headroom for any sane
  // sample size / threshold choice.
  ExpiryManager::ActiveCycleStats last{};
  for (int cycle = 0; cycle < 200 && f.evicted.size() < static_cast<std::size_t>(kKeys);
       ++cycle) {
    last = f.expiry.active_cycle(/*budget_us=*/2000);
    REQUIRE_FALSE(last.not_implemented);
  }
  CHECK(f.evicted.size() >= static_cast<std::size_t>(kKeys * 95 / 100));
}

TEST_CASE("cp3: active cycle on an empty TTL set is a cheap no-op") {
  Fixture f;
  auto stats = f.expiry.active_cycle(1000);
  REQUIRE_FALSE(stats.not_implemented);
  CHECK(stats.expired == 0);
}
