#include "netlab/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>

#include <cstring>
#include <stdexcept>

namespace netlab {

uint16_t parse_port(const std::string& text) {
  size_t used = 0;
  unsigned long value = 0;
  try {
    value = std::stoul(text, &used);
  } catch (const std::exception&) {
    used = 0;
  }
  if (used != text.size() || value == 0 || value > 65535) {
    throw std::invalid_argument("invalid port: " + text);
  }
  return static_cast<uint16_t>(value);
}

void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

void set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) throw_errno("fcntl(O_NONBLOCK)");
}

namespace {

Fd bind_any(int type, uint16_t port) {
  Fd fd(::socket(AF_INET6, type, 0));
  if (!fd.valid()) throw_errno("socket");

  int on = 1;
  int off = 0;
  // SO_REUSEADDR lets a restarted server bind while old connections sit in TIME_WAIT.
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  // IPV6_V6ONLY=0 makes the IPv6 socket accept IPv4 clients as ::ffff:a.b.c.d.
  ::setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);

  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons(port);  // network byte order (big-endian) on the wire
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    throw_errno("bind port " + std::to_string(port));
  }
  return fd;
}

Fd connect_any(int type, const std::string& host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = type;
  addrinfo* results = nullptr;
  int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);
  if (rc != 0) throw std::runtime_error("resolve " + host + ": " + ::gai_strerror(rc));

  int last_errno = 0;
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    Fd fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (fd.valid() && ::connect(fd.get(), ai->ai_addr, ai->ai_addrlen) == 0) {
      ::freeaddrinfo(results);
      return fd;
    }
    last_errno = errno;
  }
  ::freeaddrinfo(results);
  errno = last_errno;
  throw_errno("connect " + host + ":" + std::to_string(port));
}

}  // namespace

Fd tcp_listen(uint16_t port, int backlog) {
  Fd fd = bind_any(SOCK_STREAM, port);
  // backlog = how many completed handshakes the kernel queues before accept().
  if (::listen(fd.get(), backlog) < 0) throw_errno("listen");
  return fd;
}

Fd udp_bind(uint16_t port) { return bind_any(SOCK_DGRAM, port); }

Fd tcp_connect(const std::string& host, uint16_t port) {
  return connect_any(SOCK_STREAM, host, port);
}

Fd udp_connect(const std::string& host, uint16_t port) {
  return connect_any(SOCK_DGRAM, host, port);
}

bool send_all(int fd, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  while (len > 0) {
    ssize_t n = ::send(fd, p, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

std::string to_string(const sockaddr_storage& addr) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (addr.ss_family == AF_INET) {
    const auto& a = reinterpret_cast<const sockaddr_in&>(addr);
    ::inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
  }
  if (addr.ss_family == AF_INET6) {
    const auto& a = reinterpret_cast<const sockaddr_in6&>(addr);
    uint16_t port = ntohs(a.sin6_port);
    if (IN6_IS_ADDR_V4MAPPED(&a.sin6_addr)) {
      ::inet_ntop(AF_INET, &a.sin6_addr.s6_addr[12], buf, sizeof buf);
      return std::string(buf) + ":" + std::to_string(port);
    }
    ::inet_ntop(AF_INET6, &a.sin6_addr, buf, sizeof buf);
    return "[" + std::string(buf) + "]:" + std::to_string(port);
  }
  return "<unknown family " + std::to_string(addr.ss_family) + ">";
}

}  // namespace netlab
