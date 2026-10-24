// Step 2 solution: an HTTP/1.0 client on the raw sockets API.
//
//   step2_client HOST PORT [PATH]
//
// getaddrinfo → socket → connect (trying every address) → send the request
// (looping over short writes) → recv until EOF → print exactly the bytes received.
// Exit codes: 0 ok, 1 usage, 2 resolve/connect/IO error.

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "netlab/fd.hpp"

namespace {

// Connects to the first address of HOST:PORT that accepts. getaddrinfo may return
// several (IPv6 and IPv4 for "localhost", many A records for a big site), and the
// first one isn't always reachable.
netlab::Fd connect_to(const char* host, const char* port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;      // IPv4 or IPv6, whatever the name has
  hints.ai_socktype = SOCK_STREAM;  // TCP
  addrinfo* results = nullptr;
  if (int rc = ::getaddrinfo(host, port, &hints, &results); rc != 0) {
    std::fprintf(stderr, "resolve %s: %s\n", host, ::gai_strerror(rc));
    return netlab::Fd();
  }

  netlab::Fd sock;
  int last_errno = 0;
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    netlab::Fd candidate(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (!candidate.valid()) {
      last_errno = errno;
      continue;
    }
    // connect() sends the SYN and blocks until SYN-ACK (success), RST
    // (ECONNREFUSED) or the kernel gives up retransmitting (ETIMEDOUT).
    if (::connect(candidate.get(), ai->ai_addr, ai->ai_addrlen) == 0) {
      sock = std::move(candidate);
      break;
    }
    last_errno = errno;  // candidate closes itself here; try the next address
  }
  ::freeaddrinfo(results);
  if (!sock.valid()) std::fprintf(stderr, "connect %s:%s: %s\n", host, port, std::strerror(last_errno));
  return sock;
}

// send() may accept fewer bytes than asked (the socket buffer was full), and it
// may be interrupted by a signal. Loop until everything is handed to the kernel.
bool send_all(int fd, const char* data, size_t len) {
  while (len > 0) {
    ssize_t n = ::send(fd, data, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr, "usage: %s HOST PORT [PATH]\n", argv[0]);
    return 1;
  }
  const char* host = argv[1];
  const char* path = argc == 4 ? argv[3] : "/";

  netlab::Fd sock = connect_to(host, argv[2]);
  if (!sock.valid()) return 2;

  std::string request = std::string("GET ") + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
  if (!send_all(sock.get(), request.data(), request.size())) {
    std::fprintf(stderr, "send: %s\n", std::strerror(errno));
    return 2;
  }

  // HTTP/1.0: the server closes the connection after the response, so we read
  // until recv() returns 0 (the server's FIN). Each recv returns whatever has
  // arrived so far, anywhere from 1 byte to sizeof buf; never assume more.
  char buf[4096];
  for (;;) {
    ssize_t n = ::recv(sock.get(), buf, sizeof buf, 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "recv: %s\n", std::strerror(errno));
      return 2;
    }
    std::fwrite(buf, 1, static_cast<size_t>(n), stdout);
  }
  return 0;
}
