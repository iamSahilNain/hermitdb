// CHECKPOINT 4 acceptance suite (unit tier). FAILS until persist/wal.cpp is
// implemented. The crash-consistency kill -9 harness lives in
// tests/integration/test_kill9_recovery.py.
//   ctest --test-dir build -L cp4 --output-on-failure

#include "persist/wal.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hermit;
using persist::FsyncPolicy;
using persist::Wal;
using persist::WalStatus;
using protocol::Command;

namespace {

struct TmpDir {
  std::filesystem::path path;
  TmpDir() {
    path = std::filesystem::temp_directory_path() /
           ("hermit_wal_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(path);
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string wal() const { return (path / "wal.resp").string(); }
};

std::vector<Command> replay_all(Wal& wal, WalStatus& status) {
  std::vector<Command> out;
  status = wal.replay([&out](const Command& c) { out.push_back(c); });
  return out;
}

}  // namespace

TEST_CASE("cp4: append then replay round-trips commands in order") {
  TmpDir tmp;
  const std::vector<Command> workload = {
      {"SET", "a", "1"},
      {"SET", "b", "hello world"},
      {"DEL", "a"},
      {"SET", "bin", std::string("x\r\ny", 4)},  // binary-safe payloads
      {"EXPIRE", "b", "100"},
  };

  {
    Wal wal({tmp.wal(), FsyncPolicy::kAlways});
    REQUIRE(wal.open_for_append() == WalStatus::kOk);
    for (const auto& cmd : workload) REQUIRE(wal.append(cmd) == WalStatus::kOk);
  }

  Wal wal({tmp.wal(), FsyncPolicy::kAlways});
  WalStatus st{};
  auto replayed = replay_all(wal, st);
  REQUIRE(st == WalStatus::kOk);
  CHECK(replayed == workload);
}

TEST_CASE("cp4: replaying an absent or empty log yields nothing, cleanly") {
  TmpDir tmp;
  SECTION("absent file") {
    Wal wal({tmp.wal(), FsyncPolicy::kNo});
    WalStatus st{};
    auto replayed = replay_all(wal, st);
    REQUIRE(st == WalStatus::kOk);
    CHECK(replayed.empty());
  }
  SECTION("empty file") {
    std::ofstream(tmp.wal()).close();
    Wal wal({tmp.wal(), FsyncPolicy::kNo});
    WalStatus st{};
    auto replayed = replay_all(wal, st);
    REQUIRE(st == WalStatus::kOk);
    CHECK(replayed.empty());
  }
}

TEST_CASE("cp4: a torn final record is tolerated — crash mid-append is normal") {
  TmpDir tmp;
  {
    Wal wal({tmp.wal(), FsyncPolicy::kAlways});
    REQUIRE(wal.open_for_append() == WalStatus::kOk);
    REQUIRE(wal.append({"SET", "k1", "v1"}) == WalStatus::kOk);
    REQUIRE(wal.append({"SET", "k2", "v2"}) == WalStatus::kOk);
  }
  // Simulate the crash: chop bytes off the tail so the final record is torn.
  auto size = std::filesystem::file_size(tmp.wal());
  REQUIRE(size > 5);
  std::filesystem::resize_file(tmp.wal(), size - 5);

  Wal wal({tmp.wal(), FsyncPolicy::kAlways});
  WalStatus st{};
  auto replayed = replay_all(wal, st);
  REQUIRE(st == WalStatus::kOk);  // NOT kCorrupt — torn tail is expected
  CHECK(replayed == std::vector<Command>{{"SET", "k1", "v1"}});
}

TEST_CASE("cp4: appends after recovery continue the log") {
  TmpDir tmp;
  {
    Wal wal({tmp.wal(), FsyncPolicy::kEverySec});
    REQUIRE(wal.open_for_append() == WalStatus::kOk);
    REQUIRE(wal.append({"SET", "a", "1"}) == WalStatus::kOk);
    REQUIRE(wal.tick_fsync(/*now_ms=*/1000) == WalStatus::kOk);
  }
  {
    Wal wal({tmp.wal(), FsyncPolicy::kEverySec});
    WalStatus st{};
    replay_all(wal, st);
    REQUIRE(st == WalStatus::kOk);
    REQUIRE(wal.open_for_append() == WalStatus::kOk);
    REQUIRE(wal.append({"SET", "b", "2"}) == WalStatus::kOk);
  }
  Wal wal({tmp.wal(), FsyncPolicy::kEverySec});
  WalStatus st{};
  auto replayed = replay_all(wal, st);
  REQUIRE(st == WalStatus::kOk);
  CHECK(replayed == std::vector<Command>{{"SET", "a", "1"}, {"SET", "b", "2"}});
}

TEST_CASE("cp4: rewrite compacts — snapshot written atomically, wal truncated") {
  TmpDir tmp;
  Wal wal({tmp.wal(), FsyncPolicy::kAlways});
  REQUIRE(wal.open_for_append() == WalStatus::kOk);
  for (int i = 0; i < 100; ++i)
    REQUIRE(wal.append({"SET", "k" + std::to_string(i), "v"}) == WalStatus::kOk);

  bool snapshot_written = false;
  REQUIRE(wal.rewrite([&](const std::string& tmp_path) {
            // Claude Code's snapshot serializer stands in here; CP4 owns the
            // ordering/atomicity around this call, not its contents.
            std::ofstream f(tmp_path, std::ios::binary);
            f << "FAKE-SNAPSHOT";
            snapshot_written = true;
            return f.good();
          }) == WalStatus::kOk);
  CHECK(snapshot_written);

  WalStatus st{};
  auto replayed = replay_all(wal, st);
  REQUIRE(st == WalStatus::kOk);
  CHECK(replayed.empty());  // log was truncated; state now lives in the snapshot
}
