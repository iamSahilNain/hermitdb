#include "core/shard.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/commands.h"
#include "core/db.h"
#include "core/expiry.h"
#include "net/event_loop.h"
#include "net/listener.h"
#include "persist/snapshot.h"
#include "persist/wal.h"
#include "util/clock.h"
#include "util/log.h"

namespace hermit::core {

ShardedServer::ShardedServer(const Config& cfg) : cfg_(cfg) {}

ShardStatus ShardedServer::start_and_run() {
  // ==== CHECKPOINT 5: YOUR CODE ====
  SystemClock clock;
  Db db;
  ExpiryManager expiry(db, clock);
  CommandDispatcher dispatcher(db, expiry, clock);

  // The one lock. Everything that touches the keyspace, the TTL table, or the
  // WAL takes it; everything else (I/O, parsing, encoding) stays outside.
  std::mutex keyspace_mu;

  std::unique_ptr<persist::Wal> wal;
  persist::Wal* logging_wal = nullptr;
  expiry.set_evict_hook([&db, &logging_wal](const std::string& key) {
    db.del(key);
    if (logging_wal != nullptr) logging_wal->append({"DEL", key});
  });

  // NOTE: this recovery sequence mirrors main.cpp's single-threaded path.
  // Both are short and neither owns the other; if a third caller ever appears
  // this is the thing to lift into a helper.
  if (cfg_.wal_enabled) {
    std::error_code ec;
    std::filesystem::create_directories(cfg_.data_dir, ec);
    if (ec) {
      logging::error("cannot create data dir %s: %s", cfg_.data_dir.c_str(),
                     ec.message().c_str());
      return ShardStatus::kFatalError;
    }
    if (std::filesystem::exists(cfg_.snapshot_path())) {
      std::string serr;
      if (!persist::load_snapshot(db, expiry, cfg_.snapshot_path(), &serr)) {
        logging::error("snapshot load failed: %s", serr.c_str());
        return ShardStatus::kFatalError;
      }
    }
    wal = std::make_unique<persist::Wal>(persist::Wal::Options{cfg_.wal_path(), *cfg_.fsync});
    const persist::WalStatus st =
        wal->replay([&](const protocol::Command& cmd) { dispatcher.execute(cmd); });
    if (st != persist::WalStatus::kOk) {
      logging::error("wal recovery failed (corrupt=%d)", st == persist::WalStatus::kCorrupt);
      return ShardStatus::kFatalError;
    }
    if (wal->open_for_append() != persist::WalStatus::kOk) return ShardStatus::kFatalError;
    logging_wal = wal.get();
    dispatcher.set_wal(wal.get());
  }

  net::Listener listener;
  std::string err;
  if (!listener.open(cfg_.port, /*backlog=*/511, &err)) {
    logging::error("cannot listen on :%u — %s", cfg_.port, err.c_str());
    return ShardStatus::kFatalError;
  }

  std::vector<std::unique_ptr<net::EventLoop>> loops;
  loops.reserve(cfg_.nthreads);
  for (unsigned i = 0; i < cfg_.nthreads; ++i)
    loops.push_back(std::make_unique<net::EventLoop>(listener, cfg_));

  const auto stop_all = [&loops] {
    for (auto& l : loops) l->stop();
  };

  for (unsigned i = 0; i < cfg_.nthreads; ++i) {
    net::EventLoop* lp = loops[i].get();
    lp->set_handler([&keyspace_mu, &dispatcher, &stop_all](const protocol::Command& cmd) {
      std::lock_guard<std::mutex> guard(keyspace_mu);
      std::string reply = dispatcher.execute(cmd);
      if (dispatcher.shutdown_requested()) stop_all();
      return reply;
    });
    // Background maintenance runs on exactly one loop. Running it on all of
    // them would multiply the work and the lock traffic without expiring a
    // single extra key.
    if (i == 0) {
      lp->add_tick(
          [&keyspace_mu, &expiry, &wal, &clock] {
            std::lock_guard<std::mutex> guard(keyspace_mu);
            expiry.active_cycle(/*budget_us=*/1000);
            if (wal) wal->tick_fsync(clock.wall_ms());
          },
          /*interval_ms=*/100);
    }
  }

  logging::info("hermitdb listening on :%u (%u reactor threads, shared keyspace)", cfg_.port,
                cfg_.nthreads);

  std::atomic<bool> fatal{false};
  std::vector<std::thread> workers;
  workers.reserve(cfg_.nthreads - 1);
  for (unsigned i = 1; i < cfg_.nthreads; ++i) {
    workers.emplace_back([lp = loops[i].get(), &fatal] {
      if (lp->run() == net::LoopStatus::kFatalError) fatal.store(true);
    });
  }

  // Thread 0 is this thread: one fewer context to schedule, and the process
  // stays attached to its loop for signals and clean teardown.
  if (loops[0]->run() == net::LoopStatus::kFatalError) fatal.store(true);
  stop_all();
  for (auto& t : workers) t.join();

  return fatal.load() ? ShardStatus::kFatalError : ShardStatus::kStopped;
  // ==== END CHECKPOINT 5 ====
}

}  // namespace hermit::core
