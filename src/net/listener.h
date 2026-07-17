#pragma once
// Listening socket setup. [Claude Code] — plumbing, not a checkpoint.
// CP2 (the event loop) consumes this: fd() goes into epoll, accept_client()
// is called when the listener becomes readable.

#include <cstdint>
#include <string>

namespace hermit::net {

// O_NONBLOCK via fcntl. Returns false on failure (errno preserved).
bool set_nonblocking(int fd);

class Listener {
 public:
  Listener() = default;
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // socket() + SO_REUSEADDR + bind(0.0.0.0:port) + listen(); the listening fd
  // is nonblocking (mandatory for the CP2 loop — see the accept storm note in
  // checkpoints/CP2.md). Returns false with a message in *err on failure.
  bool open(uint16_t port, int backlog = 511, std::string* err = nullptr);

  int fd() const { return fd_; }

  // Accepts ONE pending connection. Returns the new fd, already nonblocking
  // with TCP_NODELAY set. Returns -1 with would_block=true when the accept
  // queue is drained (EAGAIN) — the CP2 loop should accept in a loop until
  // this — and -1 with would_block=false on real errors.
  int accept_client(bool& would_block);

 private:
  int fd_ = -1;
};

}  // namespace hermit::net
