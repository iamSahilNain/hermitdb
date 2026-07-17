#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "persist/wal.h"  // FsyncPolicy
#include "util/log.h"

namespace hermit {

struct Config {
  uint16_t port = 6380;

  // DECISION-4 pending: >1 requires CP5. main() refuses to start multi-threaded
  // until the ShardedServer checkpoint is implemented.
  unsigned nthreads = 1;

  std::string data_dir = "./data";

  // Persistence is off until CP4 lands (M5). When --wal is given, --fsync is
  // REQUIRED: DECISION-2 (the default policy) is deliberately unresolved, so
  // the code refuses to pick one silently.
  bool wal_enabled = false;
  std::optional<persist::FsyncPolicy> fsync;

  // DECISION-6 pending: provisional DoS-surface numbers; revisit before M7.
  std::size_t max_frame_bytes = 64u << 20;  // 64 MiB
  int max_clients = 10000;

  logging::Level log_level = logging::Level::kInfo;

  std::string wal_path() const { return data_dir + "/wal.resp"; }
  std::string snapshot_path() const { return data_dir + "/snapshot.hdb"; }
};

// Parses --key=value flags. Returns false and sets `err` on any problem
// (unknown flag, bad value, unresolved-decision violations). `--help` also
// returns false with the usage text in `err`.
bool parse_args(const std::vector<std::string>& args, Config& cfg, std::string& err);
bool parse_args(int argc, char** argv, Config& cfg, std::string& err);

}  // namespace hermit
