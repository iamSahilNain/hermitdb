#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "protocol/resp_parser.h"

namespace hermit::persist {

// DECISION-2 pending: the DEFAULT policy is unresolved — config refuses to
// pick one silently ("--wal requires an explicit --fsync=...").
enum class FsyncPolicy { kAlways, kEverySec, kNo };

enum class WalStatus {
  kOk,
  kIoError,
  kCorrupt,         // replay: irrecoverable damage BEFORE the final record
  kNotImplemented,  // stub — CHECKPOINT 4 pending
};

// ============================= CHECKPOINT 4 =================================
// Write-ahead log. Study sheet: checkpoints/CP4.md
//
// CONTRACT
//  - append(cmd): serialize the mutating command in RESP array encoding
//    (reuse resp_writer — the WAL format IS the wire format, so replay can
//    share the parser you wrote in CP1) and append to the log file. When the
//    append happens relative to the client reply — post-validation
//    pre-reply, or post-reply — is YOUR trade-off to reason about; document
//    it in DECISIONS.md under DECISION-2's durability analysis.
//  - fsync per policy: kAlways = every append; kEverySec = tick_fsync()
//    (called from the event-loop tick) syncs at most once per second;
//    kNo = leave it to the kernel.
//  - replay(fn): stream the log from the start, invoking fn per command.
//    A TORN FINAL RECORD (crash mid-append) is NOT corruption: stop there,
//    truncate the tail, return kOk. Damage before the final record is
//    kCorrupt.
//  - rewrite(write_snapshot): compaction. write_snapshot(tmp_path) writes a
//    full snapshot to tmp_path (Claude Code's serializer, persist/snapshot).
//    YOU own ordering + atomicity: temp file, fsync it, rename() over the
//    real snapshot, fsync the DIRECTORY, then truncate the WAL. Document the
//    crash window at every step.
//  - Boot sequence (wired in main() at M5): load snapshot, then replay WAL
//    tail on top.
// ============================================================================
class Wal {
 public:
  struct Options {
    std::string path;
    FsyncPolicy policy;
  };

  explicit Wal(Options opts);
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  WalStatus open_for_append();
  WalStatus append(const protocol::Command& cmd);
  // Drives kEverySec from the event-loop tick; no-op under other policies.
  WalStatus tick_fsync(int64_t now_ms);
  WalStatus replay(const std::function<void(const protocol::Command&)>& fn);
  WalStatus rewrite(const std::function<bool(const std::string& tmp_path)>& write_snapshot);

 private:
  // ==== CHECKPOINT 4: YOUR CODE ====
  Options opts_;
  // ==== END CHECKPOINT 4 ====
};

}  // namespace hermit::persist
