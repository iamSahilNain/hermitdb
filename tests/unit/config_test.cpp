#include "util/config.h"

#include <catch2/catch_test_macros.hpp>

using namespace hermit;

TEST_CASE("defaults") {
  Config cfg;
  std::string err;
  REQUIRE(parse_args({}, cfg, err));
  CHECK(cfg.port == 6380);
  CHECK(cfg.nthreads == 1);
  CHECK_FALSE(cfg.wal_enabled);
  CHECK_FALSE(cfg.fsync.has_value());
}

TEST_CASE("flags parse") {
  Config cfg;
  std::string err;
  REQUIRE(parse_args({"--port=7000", "--threads=4", "--data-dir=/tmp/h", "--wal",
                      "--fsync=everysec", "--max-frame-bytes=1048576", "--log-level=debug"},
                     cfg, err));
  CHECK(cfg.port == 7000);
  CHECK(cfg.nthreads == 4);
  CHECK(cfg.data_dir == "/tmp/h");
  CHECK(cfg.wal_enabled);
  REQUIRE(cfg.fsync.has_value());
  CHECK(*cfg.fsync == persist::FsyncPolicy::kEverySec);
  CHECK(cfg.max_frame_bytes == 1048576);
  CHECK(cfg.wal_path() == "/tmp/h/wal.resp");
}

TEST_CASE("fsync policy values") {
  Config cfg;
  std::string err;
  REQUIRE(parse_args({"--fsync=always"}, cfg, err));
  CHECK(*cfg.fsync == persist::FsyncPolicy::kAlways);
  REQUIRE(parse_args({"--fsync=no"}, cfg, err));
  CHECK(*cfg.fsync == persist::FsyncPolicy::kNo);
  CHECK_FALSE(parse_args({"--fsync=sometimes"}, cfg, err));
}

TEST_CASE("wal without fsync is refused — DECISION-2 must be explicit") {
  Config cfg;
  std::string err;
  CHECK_FALSE(parse_args({"--wal"}, cfg, err));
  CHECK(err.find("DECISION-2") != std::string::npos);
}

TEST_CASE("bad input is rejected") {
  Config cfg;
  std::string err;
  CHECK_FALSE(parse_args({"--port=0"}, cfg, err));
  CHECK_FALSE(parse_args({"--port=99999"}, cfg, err));
  CHECK_FALSE(parse_args({"--port=abc"}, cfg, err));
  CHECK_FALSE(parse_args({"--threads=0"}, cfg, err));
  CHECK_FALSE(parse_args({"--no-such-flag=1"}, cfg, err));
  CHECK_FALSE(parse_args({"positional"}, cfg, err));
  CHECK_FALSE(parse_args({"--help"}, cfg, err));
}
