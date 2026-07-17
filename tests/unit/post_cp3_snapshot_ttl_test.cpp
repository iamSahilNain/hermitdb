// post-cp3: TTL deadlines must survive a snapshot round-trip. The snapshot
// serializer (Claude Code) is complete; this FAILS until CHECKPOINT 3 makes
// ExpiryManager actually store deadlines. Informational in CI until then.

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>

#include "core/db.h"
#include "core/expiry.h"
#include "persist/snapshot.h"
#include "util/clock.h"

using namespace hermit;

TEST_CASE("post-cp3: snapshot round-trips TTL deadlines") {
  auto dir = std::filesystem::temp_directory_path() /
             ("hermit_snapttl_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "snap.hdb").string();

  core::Db db;
  ManualClock clock;
  core::ExpiryManager expiry(db, clock);
  db.set("ttl-key", "v");
  db.set("plain", "v");
  expiry.set_expiry("ttl-key", 123456);

  std::string err;
  REQUIRE(persist::save_snapshot(db, expiry, path, &err));

  core::Db db2;
  core::ExpiryManager expiry2(db2, clock);
  REQUIRE(persist::load_snapshot(db2, expiry2, path, &err));

  REQUIRE(expiry2.expiry_at("ttl-key").has_value());  // fails on the CP3 stub
  CHECK(*expiry2.expiry_at("ttl-key") == 123456);
  CHECK_FALSE(expiry2.expiry_at("plain").has_value());

  std::filesystem::remove_all(dir);
}
