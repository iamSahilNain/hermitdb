#include "net/event_loop.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "protocol/resp_writer.h"
#include "util/log.h"

namespace hermit::net {
namespace {

constexpr std::size_t kReadChunk = 64 * 1024;
// Fairness cap. Under LT a partial drain simply re-fires on the next
// epoll_wait, so we can bound how long one loud client holds the loop without
// risking the ET starvation bug.
constexpr std::size_t kMaxReadPerEvent = 1 << 20;
// DECISION-6: a client that stops reading must not be able to make us buffer
// unboundedly on its behalf. Redis calls this client-output-buffer-limit.
constexpr std::size_t kMaxOutbufBytes = 256u << 20;
constexpr int kMaxLoopTimeoutMs = 100;

int64_t steady_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

EventLoop::EventLoop(Listener& listener, const Config& cfg) : listener_(listener), cfg_(cfg) {}

EventLoop::~EventLoop() {
  if (epfd_ >= 0) ::close(epfd_);
}

bool EventLoop::register_fd(int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
    logging::error("epoll_ctl(ADD, %d) failed: %s", fd, std::strerror(errno));
    return false;
  }
  interest_[fd] = events;
  return true;
}

bool EventLoop::modify_fd(int fd, uint32_t events) {
  auto it = interest_.find(fd);
  if (it != interest_.end() && it->second == events) return true;  // already armed
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
    logging::error("epoll_ctl(MOD, %d) failed: %s", fd, std::strerror(errno));
    return false;
  }
  interest_[fd] = events;
  return true;
}

void EventLoop::close_connection(int fd) {
  ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  interest_.erase(fd);
  conns_.erase(fd);  // ~Connection closes the fd
  closed_this_batch_.push_back(fd);
}

void EventLoop::accept_new_clients() {
  // Drain the accept queue: one epoll wakeup can cover many pending SYNs, and
  // leaving them queued costs a wakeup each.
  for (;;) {
    bool would_block = false;
    const int fd = listener_.accept_client(would_block);
    if (fd < 0) {
      if (!would_block)
        logging::warn("accept failed: %s", std::strerror(errno));
      return;
    }
    if (static_cast<int>(conns_.size()) >= cfg_.max_clients) {
      static const std::string kFull = resp::error("ERR max number of clients reached");
      ::send(fd, kFull.data(), kFull.size(), MSG_NOSIGNAL);
      ::close(fd);
      logging::warn("refused connection: max_clients=%d reached", cfg_.max_clients);
      continue;
    }
    auto conn = std::make_unique<Connection>(fd, cfg_.max_frame_bytes);
    if (!register_fd(fd, EPOLLIN)) continue;  // ~Connection closes fd
    conns_.emplace(fd, std::move(conn));
  }
}

bool EventLoop::flush(Connection& conn) {
  const int fd = conn.fd();
  while (!conn.outbuf().empty()) {
    const ssize_t n = ::send(fd, conn.outbuf().data(), conn.outbuf().size(), MSG_NOSIGNAL);
    if (n > 0) {
      conn.consume_outbuf(static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // kernel buffer full
    close_connection(fd);
    return false;
  }

  if (conn.outbuf().empty()) {
    if (conn.close_after_flush()) {
      close_connection(fd);
      return false;
    }
    modify_fd(fd, EPOLLIN);
    return true;
  }
  // Short write: park the remainder and ask to be told when the socket drains.
  // This is the whole slow-reader story — the bytes wait on us, not the loop.
  if (conn.outbuf().size() > kMaxOutbufBytes) {
    logging::warn("client fd=%d exceeded output buffer limit; dropping", fd);
    close_connection(fd);
    return false;
  }
  modify_fd(fd, EPOLLIN | EPOLLOUT);
  return true;
}

bool EventLoop::handle_readable(Connection& conn) {
  const int fd = conn.fd();
  std::size_t read_this_event = 0;

  for (;;) {
    const ssize_t n = ::recv(fd, scratch_.data(), scratch_.size(), 0);
    if (n > 0) {
      read_this_event += static_cast<std::size_t>(n);
      protocol::ParseResult r =
          conn.parser().feed(std::string_view(scratch_.data(), static_cast<std::size_t>(n)));
      if (r.status != protocol::ParseStatus::kOk) {
        conn.queue_reply(resp::error(r.error.empty() ? "ERR Protocol error" : r.error));
        conn.mark_close_after_flush();
        break;  // no point reading more from a connection we are about to drop
      }
      for (const auto& cmd : r.commands) conn.queue_reply(handler_(cmd));
      if (stop_requested_) break;  // SHUTDOWN: stop pulling new work
      if (static_cast<std::size_t>(n) < scratch_.size()) break;  // socket drained
      if (read_this_event >= kMaxReadPerEvent) break;            // yield; LT re-fires
      continue;
    }
    if (n == 0) {  // orderly peer close
      close_connection(fd);
      return false;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    // ECONNRESET and friends: the rude-disconnect case in the slow-reader test.
    close_connection(fd);
    return false;
  }

  return flush(conn);
}

int64_t EventLoop::run_due_ticks(int64_t now_ms) {
  int64_t next_due = now_ms + kMaxLoopTimeoutMs;
  for (auto& t : tick_state_) {
    if (now_ms >= t.next_due_ms) {
      t.fn();
      // Anchor to now rather than to the old deadline: a slow tick must not
      // build up a backlog it then fires back-to-back to catch up.
      t.next_due_ms = now_ms + t.interval_ms;
    }
    next_due = std::min(next_due, t.next_due_ms);
  }
  return next_due;
}

LoopStatus EventLoop::run() {
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ < 0) {
    logging::error("epoll_create1 failed: %s", std::strerror(errno));
    return LoopStatus::kFatalError;
  }
  if (!register_fd(listener_.fd(), EPOLLIN)) return LoopStatus::kFatalError;

  scratch_.resize(kReadChunk);
  const int64_t start_ms = steady_ms();
  tick_state_.clear();
  for (auto& [fn, interval] : ticks_) tick_state_.push_back({fn, interval, start_ms + interval});

  std::vector<epoll_event> events(256);

  while (!stop_requested_) {
    const int64_t now = steady_ms();
    int64_t next_due = now + kMaxLoopTimeoutMs;
    for (const auto& t : tick_state_) next_due = std::min(next_due, t.next_due_ms);
    const int timeout = static_cast<int>(std::clamp<int64_t>(next_due - now, 0, kMaxLoopTimeoutMs));

    const int n = ::epoll_wait(epfd_, events.data(), static_cast<int>(events.size()), timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      logging::error("epoll_wait failed: %s", std::strerror(errno));
      return LoopStatus::kFatalError;
    }

    closed_this_batch_.clear();
    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      const uint32_t ev = events[i].events;

      if (fd == listener_.fd()) {
        accept_new_clients();
        continue;
      }
      // The fd may have been closed earlier in this same batch, and its number
      // may already belong to a connection accepted moments ago.
      if (std::find(closed_this_batch_.begin(), closed_this_batch_.end(), fd) !=
          closed_this_batch_.end())
        continue;

      auto it = conns_.find(fd);
      if (it == conns_.end()) continue;
      Connection& conn = *it->second;

      if (ev & (EPOLLHUP | EPOLLERR)) {
        close_connection(fd);
        continue;
      }
      if (ev & EPOLLIN) {
        if (!handle_readable(conn)) continue;  // destroyed; conn is dangling
      }
      if (ev & EPOLLOUT) {
        if (!flush(conn)) continue;
      }
    }

    run_due_ticks(steady_ms());

    // A full event array means we probably left readiness on the table.
    if (n == static_cast<int>(events.size()) && events.size() < 4096) events.resize(events.size() * 2);
  }

  logging::info("event loop stopping; %zu connection(s) open", conns_.size());
  conns_.clear();
  interest_.clear();
  return LoopStatus::kStopped;
}

}  // namespace hermit::net
