#pragma once
// Per-connection state. [Claude Code] — plumbing, not a checkpoint.
// The CP2 event loop owns a map fd -> Connection; it recv()s into a scratch
// buffer, feeds the parser, and queues replies here.

#include <cstddef>
#include <string>
#include <string_view>

#include "protocol/resp_parser.h"

namespace hermit::net {

class Connection {
 public:
  Connection(int fd, std::size_t max_frame_bytes);
  ~Connection();  // closes the fd
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  int fd() const { return fd_; }
  protocol::RespParser& parser() { return parser_; }

  // Reply bytes waiting for the kernel send buffer. The CP2 loop flushes this
  // on writable events; wants_write() drives EPOLLOUT registration.
  void queue_reply(std::string_view bytes) { outbuf_.append(bytes.data(), bytes.size()); }
  std::string& outbuf() { return outbuf_; }
  bool wants_write() const { return !outbuf_.empty(); }
  // Drop the first n flushed bytes (after a successful/short send()).
  void consume_outbuf(std::size_t n) { outbuf_.erase(0, n); }

  // Set after a protocol error reply is queued: flush, then close.
  void mark_close_after_flush() { close_after_flush_ = true; }
  bool close_after_flush() const { return close_after_flush_; }

 private:
  int fd_;
  protocol::RespParser parser_;
  std::string outbuf_;
  bool close_after_flush_ = false;
};

}  // namespace hermit::net
