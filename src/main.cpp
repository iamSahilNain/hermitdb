// hermitdb entrypoint: flag parsing, wiring, lifecycle. [Claude Code]

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "core/commands.h"
#include "core/db.h"
#include "core/expiry.h"
#include "core/shard.h"
#include "net/event_loop.h"
#include "net/listener.h"
#include "persist/snapshot.h"
#include "persist/wal.h"
#include "util/clock.h"
#include "util/config.h"
#include "util/log.h"

int main(int argc, char** argv) {
  using namespace hermit;

  Config cfg;
  std::string err;
  if (!parse_args(argc, argv, cfg, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  logging::set_level(cfg.log_level);

  // A client vanishing mid-reply must not take the server with it. Every
  // send() also passes MSG_NOSIGNAL; this is the belt to that suspenders.
  std::signal(SIGPIPE, SIG_IGN);

  // DECISION-4 resolved: N reactor threads over one locked keyspace (CP5).
  if (cfg.nthreads > 1) {
    core::ShardedServer sharded(cfg);
    const core::ShardStatus st = sharded.start_and_run();
    if (st == core::ShardStatus::kNotImplemented) {
      logging::error("--threads=%u unavailable until CHECKPOINT 5 (checkpoints/CP5.md)",
                     cfg.nthreads);
      return 1;
    }
    return st == core::ShardStatus::kStopped ? 0 : 1;
  }

  SystemClock clock;
  core::Db db;
  core::ExpiryManager expiry(db, clock);

  core::CommandDispatcher dispatcher(db, expiry, clock);

  std::unique_ptr<persist::Wal> wal;
  // Stays null until recovery finishes: replaying the log must not append to
  // it. Flipping this on afterwards is what arms eviction logging.
  persist::Wal* logging_wal = nullptr;
  expiry.set_evict_hook([&db, &logging_wal, &dispatcher](const std::string& key) {
    db.del(key);
    // The CP3<->CP4 contract: an expiry is a real mutation, and replay must
    // not re-derive it against a later clock. Log it as an explicit DEL.
    if (logging_wal != nullptr && !dispatcher.persistence_failed() &&
        logging_wal->append({"DEL", key}) != persist::WalStatus::kOk)
      dispatcher.report_persistence_failure();
  });

  if (cfg.wal_enabled) {
    std::error_code ec;
    std::filesystem::create_directories(cfg.data_dir, ec);
    if (ec) {
      logging::error("cannot create data dir %s: %s", cfg.data_dir.c_str(),
                     ec.message().c_str());
      return 1;
    }

    // Boot order: the snapshot is the baseline, the WAL is everything that
    // happened after it. Reversing these would replay stale mutations over
    // fresher state.
    if (std::filesystem::exists(cfg.snapshot_path())) {
      std::string serr;
      if (!persist::load_snapshot(db, expiry, cfg.snapshot_path(), &serr)) {
        logging::error("snapshot load failed: %s", serr.c_str());
        return 1;
      }
      logging::info("loaded snapshot: %zu key(s)", db.size());
    }

    wal = std::make_unique<persist::Wal>(
        persist::Wal::Options{cfg.wal_path(), *cfg.fsync});

    std::size_t replayed = 0;
    const persist::WalStatus st = wal->replay([&](const protocol::Command& cmd) {
      dispatcher.execute(cmd);  // wal_ still unset: replay cannot re-log
      ++replayed;
    });
    if (st == persist::WalStatus::kCorrupt) {
      logging::error("wal is corrupt — refusing to start with a guessed state");
      return 1;
    }
    if (st != persist::WalStatus::kOk) {
      logging::error("wal replay failed");
      return 1;
    }
    if (replayed > 0) logging::info("replayed %zu wal record(s)", replayed);

    if (wal->open_for_append() != persist::WalStatus::kOk) return 1;
    logging_wal = wal.get();
    dispatcher.set_wal(wal.get());
    logging::info("persistence on: %s", cfg.wal_path().c_str());
  }

  net::Listener listener;
  if (!listener.open(cfg.port, /*backlog=*/511, &err)) {
    logging::error("cannot listen on :%u — %s", cfg.port, err.c_str());
    return 1;
  }
  logging::info("hermitdb listening on :%u (single-threaded)", cfg.port);

  net::EventLoop loop(listener, cfg);
  loop.set_handler([&](const protocol::Command& cmd) {
    std::string reply = dispatcher.execute(cmd);
    if (dispatcher.shutdown_requested()) loop.stop();
    return reply;
  });
  loop.add_tick([&] {
    expiry.active_cycle(/*budget_us=*/1000);
    if (wal && !dispatcher.persistence_failed() &&
        wal->tick_fsync(clock.wall_ms()) != persist::WalStatus::kOk)
      dispatcher.report_persistence_failure();
    if (dispatcher.persistence_failed()) loop.stop();
  }, /*interval_ms=*/100);

  switch (loop.run()) {
    case net::LoopStatus::kStopped:
      if (dispatcher.persistence_failed()) {
        logging::error("persistence failed; stopped to prevent unsafe acknowledgments");
        return 1;
      }
      logging::info("clean shutdown");
      return 0;
    case net::LoopStatus::kNotImplemented:
      return 1;
    case net::LoopStatus::kFatalError:
      logging::error("event loop died");
      return 1;
  }
  return 1;  // unreachable; silences -Werror=return-type on some compilers
}
