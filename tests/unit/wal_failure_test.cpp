#include <bits/stdc++.h>
using namespace std;

#include <catch2/catch_test_macros.hpp>
#include "core/commands.h"
#include "core/db.h"
#include "core/expiry.h"
#include "persist/wal.h"
#include "util/clock.h"

using namespace hermit;

TEST_CASE("WAL write failure suppresses success and latches command rejection", "[wal][failure]") {
  ManualClock clock;
  core::Db db;
  core::ExpiryManager expiry(db, clock);
  core::CommandDispatcher dispatcher(db, expiry, clock);
  persist::Wal wal({"/dev/full", persist::FsyncPolicy::kAlways});
  REQUIRE(wal.open_for_append() == persist::WalStatus::kOk);
  dispatcher.set_wal(&wal);

  REQUIRE(dispatcher.execute({"SET", "key", "value"}).rfind("-MISCONF", 0) == 0);
  REQUIRE(dispatcher.persistence_failed());
  REQUIRE(dispatcher.shutdown_requested());
  REQUIRE(dispatcher.execute({"GET", "key"}).rfind("-MISCONF", 0) == 0);
  REQUIRE(dispatcher.execute({"SET", "later", "value"}).rfind("-MISCONF", 0) == 0);
  REQUIRE_FALSE(db.exists("later"));
}

TEST_CASE("A lazy eviction failure suppresses the triggering read reply", "[wal][failure]") {
  ManualClock clock;
  core::Db db;
  core::ExpiryManager expiry(db, clock);
  core::CommandDispatcher dispatcher(db, expiry, clock);
  REQUIRE(dispatcher.execute({"SET", "key", "value", "PX", "1"}) == "+OK\r\n");
  persist::Wal wal({"/dev/full", persist::FsyncPolicy::kAlways});
  REQUIRE(wal.open_for_append() == persist::WalStatus::kOk);
  dispatcher.set_wal(&wal);
  expiry.set_evict_hook([&](const string& key) {
    db.del(key);
    if (wal.append({"DEL", key}) != persist::WalStatus::kOk)
      dispatcher.report_persistence_failure();
  });
  clock.advance(2);
  REQUIRE(dispatcher.execute({"GET", "key"}).rfind("-MISCONF", 0) == 0);
  REQUIRE(dispatcher.shutdown_requested());
}

TEST_CASE("Deferred fsync failures are exposed for the maintenance loop", "[wal][failure]") {
  // /dev/null accepts writes but fsync fails with EINVAL on Linux.
  persist::Wal wal({"/dev/null", persist::FsyncPolicy::kEverySec});
  REQUIRE(wal.open_for_append() == persist::WalStatus::kOk);
  REQUIRE(wal.append({"SET", "key", "value"}) == persist::WalStatus::kOk);
  REQUIRE(wal.tick_fsync(2000) == persist::WalStatus::kIoError);
}
