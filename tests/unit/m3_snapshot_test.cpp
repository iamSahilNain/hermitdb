// M3-scope snapshot tests: serialization round-trip for strings + lists and
// corruption rejection. TTL round-trip needs a working ExpiryManager and
// lives in post_cp3_snapshot_ttl_test.cpp. Atomicity (temp+rename+dir-fsync)
// is CP4's and is exercised by its kill -9 harness, not here.

#include "persist/snapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/db.h"
#include "core/expiry.h"
#include "util/clock.h"

using namespace hermit;
using core::Db;
using core::ExpiryManager;

namespace {

struct TmpDir {
  std::filesystem::path path;
  TmpDir() {
    path = std::filesystem::temp_directory_path() /
           ("hermit_snap_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(path);
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string file(const char* name) const { return (path / name).string(); }
};

}  // namespace

TEST_CASE("m3 snapshot: round-trips strings and lists byte-exactly") {
  TmpDir tmp;
  Db db;
  ManualClock clock;
  ExpiryManager expiry(db, clock);

  db.set("plain", "value");
  db.set("empty", "");
  db.set(std::string("bin\0key", 7), std::string("v\r\n\0\xff", 5));
  db.rpush("list", "a");
  db.rpush("list", std::string("b\0b", 3));
  db.rpush("list", "");

  std::string err;
  REQUIRE(persist::save_snapshot(db, expiry, tmp.file("snap.hdb"), &err));

  Db db2;
  ExpiryManager expiry2(db2, clock);
  REQUIRE(persist::load_snapshot(db2, expiry2, tmp.file("snap.hdb"), &err));

  CHECK(db2.size() == db.size());
  CHECK(*db2.get("plain") == "value");
  CHECK(*db2.get("empty") == "");
  CHECK(*db2.get(std::string("bin\0key", 7)) == std::string("v\r\n\0\xff", 5));
  CHECK(db2.type("list") == Db::Type::kList);
  CHECK(db2.lrange("list", 0, -1) ==
        std::vector<std::string>{"a", std::string("b\0b", 3), ""});
}

TEST_CASE("m3 snapshot: empty db round-trips") {
  TmpDir tmp;
  Db db;
  ManualClock clock;
  ExpiryManager expiry(db, clock);
  std::string err;
  REQUIRE(persist::save_snapshot(db, expiry, tmp.file("empty.hdb"), &err));
  Db db2;
  ExpiryManager expiry2(db2, clock);
  REQUIRE(persist::load_snapshot(db2, expiry2, tmp.file("empty.hdb"), &err));
  CHECK(db2.size() == 0);
}

TEST_CASE("m3 snapshot: rejects garbage without crashing or over-allocating") {
  TmpDir tmp;
  Db db;
  ManualClock clock;
  ExpiryManager expiry(db, clock);
  std::string err;

  SECTION("missing file") {
    CHECK_FALSE(persist::load_snapshot(db, expiry, tmp.file("nope.hdb"), &err));
    CHECK_FALSE(err.empty());
  }
  SECTION("bad magic") {
    std::ofstream(tmp.file("bad.hdb"), std::ios::binary) << "NOPE1234";
    CHECK_FALSE(persist::load_snapshot(db, expiry, tmp.file("bad.hdb"), &err));
    CHECK(err.find("magic") != std::string::npos);
  }
  SECTION("truncated mid-entry") {
    Db src;
    ExpiryManager src_exp(src, clock);
    src.set("key", std::string(1000, 'x'));
    REQUIRE(persist::save_snapshot(src, src_exp, tmp.file("t.hdb"), &err));
    auto size = std::filesystem::file_size(tmp.file("t.hdb"));
    std::filesystem::resize_file(tmp.file("t.hdb"), size - 500);
    CHECK_FALSE(persist::load_snapshot(db, expiry, tmp.file("t.hdb"), &err));
  }
  SECTION("length field larger than the file — must not allocate it") {
    // Header claims one entry whose key length is 4 GB-ish.
    std::ofstream f(tmp.file("evil.hdb"), std::ios::binary);
    f << "HDB1";
    uint64_t count = 1;
    f.write(reinterpret_cast<char*>(&count), 8);
    uint8_t type = 0;
    f.write(reinterpret_cast<char*>(&type), 1);
    uint32_t huge = 0xFFFFFF00u;
    f.write(reinterpret_cast<char*>(&huge), 4);
    f << "tiny";
    f.close();
    CHECK_FALSE(persist::load_snapshot(db, expiry, tmp.file("evil.hdb"), &err));
  }
}
