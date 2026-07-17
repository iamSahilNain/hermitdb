#include "net/listener.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hermit::net {

bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

Listener::~Listener() {
  if (fd_ >= 0) ::close(fd_);
}

bool Listener::open(uint16_t port, int backlog, std::string* err) {
  auto fail = [&](const char* what) {
    if (err) *err = std::string(what) + ": " + std::strerror(errno);
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    return false;
  };

  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) return fail("socket");

  int one = 1;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
    return fail("setsockopt(SO_REUSEADDR)");

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) return fail("bind");
  if (::listen(fd_, backlog) < 0) return fail("listen");
  if (!set_nonblocking(fd_)) return fail("fcntl(O_NONBLOCK)");
  return true;
}

int Listener::accept_client(bool& would_block) {
  would_block = false;
  int cfd = ::accept(fd_, nullptr, nullptr);
  if (cfd < 0) {
    would_block = (errno == EAGAIN || errno == EWOULDBLOCK);
    return -1;
  }
  if (!set_nonblocking(cfd)) {
    ::close(cfd);
    return -1;
  }
  // Latency over throughput for a request/reply protocol: don't let Nagle
  // hold small replies hostage. redis does the same.
  int one = 1;
  ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  return cfd;
}

}  // namespace hermit::net
