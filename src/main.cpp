// hermitdb entrypoint: flag parsing, wiring, lifecycle. [Claude Code]

#include <cstdio>

#include "core/commands.h"
#include "core/db.h"
#include "core/expiry.h"
#include "core/shard.h"
#include "net/event_loop.h"
#include "net/listener.h"
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

  // DECISION-4 pending: the multi-threaded path is CP5 (human).
  if (cfg.nthreads > 1) {
    core::ShardedServer sharded(cfg);
    if (sharded.start_and_run() == core::ShardStatus::kNotImplemented) {
      logging::error("--threads=%u unavailable until CHECKPOINT 5 (checkpoints/CP5.md)",
                     cfg.nthreads);
      return 1;
    }
    return 0;
  }

  SystemClock clock;
  core::Db db;
  core::ExpiryManager expiry(db, clock);
  // All expiry-driven eviction funnels through this hook; at M5 it also logs
  // a DEL to the WAL so replay is deterministic (CP3 <-> CP4 contract).
  expiry.set_evict_hook([&db](const std::string& key) { db.del(key); });
  core::CommandDispatcher dispatcher(db, expiry, clock);

  // M5 wires persistence here: load snapshot -> Wal::replay() on top ->
  // Wal::open_for_append() -> dispatcher.set_wal(&wal).

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
  // CP3 active expiry rides the loop tick; CP4 everysec fsync joins at M5.
  loop.add_tick([&expiry] { expiry.active_cycle(/*budget_us=*/1000); }, /*interval_ms=*/100);

  switch (loop.run()) {
    case net::LoopStatus::kStopped:
      logging::info("clean shutdown");
      return 0;
    case net::LoopStatus::kNotImplemented:
      // Expected until the human lands CP2 (M2). Not a scaffolding bug.
      return 1;
    case net::LoopStatus::kFatalError:
      logging::error("event loop died");
      return 1;
  }
  return 1;  // unreachable; silences -Werror=return-type on some compilers
}
