#include "persist/wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "util/log.h"

namespace hermit::persist {
namespace {

constexpr std::size_t kReplayChunk = 64 * 1024;
// Replay must accept anything append() could legally have written, so the
// parser cap here is the file's own scale, not the network's.
constexpr std::size_t kReplayFrameCap = 512u << 20;

// write() is allowed to be short even for regular files (signals, ENOSPC
// partial). Anything less than "all of it" is a torn record.
bool write_all(int fd, const char* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    const ssize_t n = ::write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

Wal::Wal(Options opts) : opts_(std::move(opts)) {}

Wal::~Wal() {
  if (fd_ >= 0) {
    if (dirty_) ::fsync(fd_);
    ::close(fd_);
  }
}

// ==== CHECKPOINT 4: YOUR CODE ====

std::string Wal::encode(const protocol::Command& cmd) {
  std::string out = "*" + std::to_string(cmd.size()) + "\r\n";
  for (const auto& arg : cmd) {
    out += "$" + std::to_string(arg.size()) + "\r\n";
    out += arg;
    out += "\r\n";
  }
  return out;
}

std::string Wal::dir() const {
  const auto slash = opts_.path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return opts_.path.substr(0, slash);
}

std::string Wal::snapshot_path() const { return dir() + "/snapshot.hdb"; }

WalStatus Wal::fsync_now() {
  if (fd_ < 0) return WalStatus::kIoError;
  if (::fsync(fd_) != 0) {
    logging::error("wal fsync failed: %s", std::strerror(errno));
    return WalStatus::kIoError;
  }
  dirty_ = false;
  return WalStatus::kOk;
}

WalStatus Wal::open_for_append() {
  if (fd_ >= 0) return WalStatus::kOk;
  // O_APPEND makes every write atomic w.r.t. the file offset, so a concurrent
  // writer (or a re-entrant eviction DEL) can never interleave inside a record.
  fd_ = ::open(opts_.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    logging::error("cannot open wal %s: %s", opts_.path.c_str(), std::strerror(errno));
    return WalStatus::kIoError;
  }
  return WalStatus::kOk;
}

WalStatus Wal::append(const protocol::Command& cmd) {
  if (fd_ < 0) return WalStatus::kIoError;
  const std::string record = encode(cmd);
  if (!write_all(fd_, record.data(), record.size())) {
    logging::error("wal write failed: %s", std::strerror(errno));
    return WalStatus::kIoError;
  }
  dirty_ = true;
  if (opts_.policy == FsyncPolicy::kAlways) return fsync_now();
  return WalStatus::kOk;
}

WalStatus Wal::tick_fsync(int64_t now_ms) {
  if (opts_.policy != FsyncPolicy::kEverySec) return WalStatus::kOk;
  if (!dirty_ || fd_ < 0) return WalStatus::kOk;
  if (now_ms - last_fsync_ms_ < 1000) return WalStatus::kOk;
  const WalStatus st = fsync_now();
  if (st == WalStatus::kOk) last_fsync_ms_ = now_ms;
  return st;
}

WalStatus Wal::replay(const std::function<void(const protocol::Command&)>& fn) {
  const int rfd = ::open(opts_.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (rfd < 0) {
    if (errno == ENOENT) return WalStatus::kOk;  // first boot: nothing to recover
    logging::error("cannot read wal %s: %s", opts_.path.c_str(), std::strerror(errno));
    return WalStatus::kIoError;
  }

  protocol::RespParser parser(kReplayFrameCap);
  std::vector<char> chunk(kReplayChunk);
  // Bytes belonging to records that parsed COMPLETELY. Re-encoding is exact
  // because append() is the only writer and its encoding is canonical, so this
  // doubles as the truncation point for a torn tail.
  std::uint64_t good_bytes = 0;
  std::uint64_t total_bytes = 0;
  bool corrupt = false;

  for (;;) {
    const ssize_t n = ::read(rfd, chunk.data(), chunk.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(rfd);
      return WalStatus::kIoError;
    }
    if (n == 0) break;
    total_bytes += static_cast<std::uint64_t>(n);

    protocol::ParseResult r = parser.feed(std::string_view(chunk.data(), static_cast<std::size_t>(n)));
    if (r.status != protocol::ParseStatus::kOk) {
      // A truncated tail can only ever look INCOMPLETE, never malformed —
      // so a parse error means damage before the end of the file.
      corrupt = true;
      break;
    }
    for (const auto& cmd : r.commands) {
      fn(cmd);
      good_bytes += encode(cmd).size();
    }
  }
  ::close(rfd);

  if (corrupt) {
    logging::error("wal %s is corrupt at byte %llu — refusing to guess",
                   opts_.path.c_str(), static_cast<unsigned long long>(good_bytes));
    return WalStatus::kCorrupt;
  }

  // Torn final record: the crash landed mid-append. Expected, not an error —
  // cut it off so the next append starts on a record boundary.
  if (good_bytes < total_bytes) {
    logging::warn("wal tail torn: dropping %llu trailing byte(s)",
                  static_cast<unsigned long long>(total_bytes - good_bytes));
    if (::truncate(opts_.path.c_str(), static_cast<off_t>(good_bytes)) != 0) {
      logging::error("cannot truncate torn wal: %s", std::strerror(errno));
      return WalStatus::kIoError;
    }
  }
  return WalStatus::kOk;
}

WalStatus Wal::rewrite(const std::function<bool(const std::string& tmp_path)>& write_snapshot) {
  const std::string snap = snapshot_path();
  const std::string tmp = snap + ".tmp";

  // Step 1: build the new snapshot beside the old one. A crash here loses
  // nothing — the old snapshot and the full WAL are both still intact.
  if (!write_snapshot(tmp)) {
    ::unlink(tmp.c_str());
    return WalStatus::kIoError;
  }

  // Step 2: force the snapshot's CONTENTS to disk before anything points at
  // it. Skipping this is the classic bug: rename() is atomic w.r.t. the
  // directory entry, which says nothing about the data blocks having landed.
  {
    const int sfd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
    if (sfd < 0 || ::fsync(sfd) != 0) {
      if (sfd >= 0) ::close(sfd);
      ::unlink(tmp.c_str());
      return WalStatus::kIoError;
    }
    ::close(sfd);
  }

  // Step 3: rename() is the atomic primitive — the snapshot name points at
  // either the whole old file or the whole new one, never a mixture.
  if (::rename(tmp.c_str(), snap.c_str()) != 0) {
    logging::error("snapshot rename failed: %s", std::strerror(errno));
    ::unlink(tmp.c_str());
    return WalStatus::kIoError;
  }

  // Step 4: the rename itself is metadata, and metadata is buffered too.
  // Without this fsync a crash can resurrect the old directory entry while
  // the WAL has already been truncated — losing everything in between.
  {
    const std::string d = dir();
    const int dfd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
      ::fsync(dfd);
      ::close(dfd);
    }
  }

  // Step 5: only now may the log go. Everything it described is durable in
  // the snapshot. A crash before this point simply replays a WAL whose
  // effects the snapshot already contains — replay is idempotent, so that is
  // harmless; a crash after it has nothing left to lose.
  if (fd_ >= 0) {
    if (::ftruncate(fd_, 0) != 0) {
      logging::error("wal truncate failed: %s", std::strerror(errno));
      return WalStatus::kIoError;
    }
    dirty_ = true;
    return fsync_now();
  }
  if (::truncate(opts_.path.c_str(), 0) != 0 && errno != ENOENT) return WalStatus::kIoError;
  return WalStatus::kOk;
}

// ==== END CHECKPOINT 4 ====

}  // namespace hermit::persist
