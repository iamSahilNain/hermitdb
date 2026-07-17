#include "net/connection.h"

#include <unistd.h>

namespace hermit::net {

Connection::Connection(int fd, std::size_t max_frame_bytes)
    : fd_(fd), parser_(max_frame_bytes) {}

Connection::~Connection() {
  if (fd_ >= 0) ::close(fd_);
}

}  // namespace hermit::net
