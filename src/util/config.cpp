#include "util/config.h"

#include <cstdlib>

namespace hermit {
namespace {

constexpr const char* kUsage =
    "hermitdb — RESP2-compatible in-memory KV store\n"
    "\n"
    "  --port=N            listen port (default 6380)\n"
    "  --threads=N         worker threads; >1 requires CP5 (default 1)\n"
    "  --data-dir=PATH     WAL + snapshot directory (default ./data)\n"
    "  --wal               enable write-ahead logging (requires --fsync)\n"
    "  --fsync=POLICY      always | everysec | no  (no default: DECISION-2)\n"
    "  --max-frame-bytes=N reject RESP frames larger than N (default 64MiB)\n"
    "  --max-clients=N     concurrent connection cap (default 10000)\n"
    "  --log-level=LEVEL   debug | info | warn | error (default info)\n"
    "  --help              this text\n";

// Splits "--key=value" into key/value; flags without '=' get value "".
bool split_flag(const std::string& arg, std::string& key, std::string& value) {
  if (arg.rfind("--", 0) != 0) return false;
  auto eq = arg.find('=');
  if (eq == std::string::npos) {
    key = arg.substr(2);
    value.clear();
  } else {
    key = arg.substr(2, eq - 2);
    value = arg.substr(eq + 1);
  }
  return true;
}

bool parse_u64(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || *end != '\0') return false;
  out = v;
  return true;
}

}  // namespace

bool parse_args(const std::vector<std::string>& args, Config& cfg, std::string& err) {
  for (const auto& arg : args) {
    std::string key, value;
    if (!split_flag(arg, key, value)) {
      err = "unexpected argument '" + arg + "'\n\n" + kUsage;
      return false;
    }
    uint64_t n = 0;
    if (key == "help") {
      err = kUsage;
      return false;
    } else if (key == "port") {
      if (!parse_u64(value, n) || n == 0 || n > 65535) {
        err = "--port must be 1..65535";
        return false;
      }
      cfg.port = static_cast<uint16_t>(n);
    } else if (key == "threads") {
      if (!parse_u64(value, n) || n == 0 || n > 256) {
        err = "--threads must be 1..256";
        return false;
      }
      cfg.nthreads = static_cast<unsigned>(n);
    } else if (key == "data-dir") {
      if (value.empty()) {
        err = "--data-dir needs a path";
        return false;
      }
      cfg.data_dir = value;
    } else if (key == "wal") {
      cfg.wal_enabled = true;
    } else if (key == "fsync") {
      if (value == "always") {
        cfg.fsync = persist::FsyncPolicy::kAlways;
      } else if (value == "everysec") {
        cfg.fsync = persist::FsyncPolicy::kEverySec;
      } else if (value == "no") {
        cfg.fsync = persist::FsyncPolicy::kNo;
      } else {
        err = "--fsync must be always|everysec|no";
        return false;
      }
    } else if (key == "max-frame-bytes") {
      if (!parse_u64(value, n) || n < 1024) {
        err = "--max-frame-bytes must be >= 1024";
        return false;
      }
      cfg.max_frame_bytes = static_cast<std::size_t>(n);
    } else if (key == "max-clients") {
      if (!parse_u64(value, n) || n == 0 || n > 1000000) {
        err = "--max-clients must be 1..1000000";
        return false;
      }
      cfg.max_clients = static_cast<int>(n);
    } else if (key == "log-level") {
      if (value == "debug") {
        cfg.log_level = logging::Level::kDebug;
      } else if (value == "info") {
        cfg.log_level = logging::Level::kInfo;
      } else if (value == "warn") {
        cfg.log_level = logging::Level::kWarn;
      } else if (value == "error") {
        cfg.log_level = logging::Level::kError;
      } else {
        err = "--log-level must be debug|info|warn|error";
        return false;
      }
    } else {
      err = "unknown flag '--" + key + "'\n\n" + kUsage;
      return false;
    }
  }

  if (cfg.wal_enabled && !cfg.fsync.has_value()) {
    // DECISION-2 pending: refusing to default is the whole point.
    err = "--wal requires an explicit --fsync=always|everysec|no (DECISION-2 is unresolved)";
    return false;
  }
  return true;
}

bool parse_args(int argc, char** argv, Config& cfg, std::string& err) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return parse_args(args, cfg, err);
}

}  // namespace hermit
