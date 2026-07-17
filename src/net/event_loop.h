#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "net/listener.h"
#include "protocol/resp_parser.h"
#include "util/config.h"

namespace hermit::net {

enum class LoopStatus {
  kStopped,         // clean shutdown via stop()
  kFatalError,      // unrecoverable (epoll_create failed, ...)
  kNotImplemented,  // stub — CHECKPOINT 2 pending
};

// ============================= CHECKPOINT 2 =================================
// Single-threaded epoll reactor. Study sheet: checkpoints/CP2.md
// DECISION-1 pending: level-triggered vs edge-triggered — resolve in
// DECISIONS.md before implementing; the choice changes the drain loops below.
//
// CONTRACT
//  - run() blocks: epoll_wait -> dispatch, until stop() or fatal error.
//  - Listener readable  => accept in a loop until EAGAIN; each new fd gets a
//    Connection (nonblocking, done by Listener::accept_client) and is
//    registered for readability. Enforce cfg.max_clients.
//  - Connection readable => recv until EAGAIN (or per LT policy), feed bytes
//    to conn.parser(), execute each complete command via handler_, queue the
//    reply. kProtocolError => queue error reply, flush, close.
//  - Writes: try to send immediately; on a short write / EAGAIN, keep the
//    remainder in conn.outbuf() and register EPOLLOUT; deregister EPOLLOUT
//    once drained (a slow reader must never block other clients — see the
//    integration test).
//  - recv()==0 (peer close) and ECONNRESET => destroy the Connection.
//  - Tick callbacks (add_tick) fire ~every interval_ms from inside the loop
//    (epoll_wait timeout is the natural mechanism). They drive CP3 active
//    expiry and CP4 everysec fsync — no extra threads in single-thread mode.
//  - stop() may be called from inside a handler (SHUTDOWN command).
// ============================================================================
class EventLoop {
 public:
  // Executes one parsed command; returns the RESP-encoded reply bytes.
  using CommandHandler = std::function<std::string(const protocol::Command&)>;
  using TickFn = std::function<void()>;

  EventLoop(Listener& listener, const Config& cfg);
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  void set_handler(CommandHandler handler) { handler_ = std::move(handler); }
  void add_tick(TickFn fn, int64_t interval_ms) { ticks_.emplace_back(std::move(fn), interval_ms); }

  LoopStatus run();
  void stop() { stop_requested_ = true; }

 private:
  // ==== CHECKPOINT 2: YOUR CODE ====
  // Your reactor state lives here: epoll fd, fd -> Connection map, tick
  // bookkeeping, scratch read buffer, ...
  Listener& listener_;
  const Config& cfg_;
  CommandHandler handler_;
  std::vector<std::pair<TickFn, int64_t>> ticks_;
  bool stop_requested_ = false;
  // ==== END CHECKPOINT 2 ====
};

}  // namespace hermit::net
