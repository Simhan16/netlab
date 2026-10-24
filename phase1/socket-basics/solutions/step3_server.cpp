// Step 3 solution: an iterative TCP server that sends back everything it
// receives, uppercased.
//
//   step3_server PORT
//
// socket → setsockopt(SO_REUSEADDR) → bind → listen → loop { accept → serve → close }.
// One client at a time: a second client's handshake completes (the kernel does
// that), but it waits in the accept queue until the first one disconnects.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "netlab/fd.hpp"

namespace {

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

// Reads until the client closes (or breaks) the connection. Any error here ends
// this client only; the server goes back to accept().
void serve(int fd) {
  char buf[4096];
  for (;;) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n == 0) return;  // client sent FIN
    if (n < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "  recv: %s\n", std::strerror(errno));  // e.g. ECONNRESET
      return;
    }
    for (ssize_t i = 0; i < n; ++i) buf[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(buf[i])));
    if (!send_all(fd, buf, static_cast<size_t>(n))) {
      std::fprintf(stderr, "  send: %s\n", std::strerror(errno));  // EPIPE, ECONNRESET
      return;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s PORT\n", argv[0]);
    return 1;
  }
  int port = std::atoi(argv[1]);

  // Writing to a connection after the kernel has already reported its reset
  // raises SIGPIPE, which kills the whole process by default. This loop stops
  // at the first error so it rarely gets there, but one sloppy code path is
  // enough: ignore it, and send() returns -1 / EPIPE instead.
  std::signal(SIGPIPE, SIG_IGN);

  netlab::Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listener.valid()) {
    std::perror("socket");
    return 2;
  }

  // Without this, restarting the server fails with "Address already in use" for
  // about a minute: the old connections sit in TIME_WAIT on this port.
  int one = 1;
  if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
    std::perror("setsockopt");
    return 2;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // every local address: 0.0.0.0
  addr.sin_port = htons(static_cast<uint16_t>(port));  // network byte order (big-endian)
  if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    std::perror("bind");
    return 2;
  }
  if (::listen(listener.get(), 16) < 0) {  // 16 = accept-queue size
    std::perror("listen");
    return 2;
  }
  std::fprintf(stderr, "listening on 0.0.0.0:%d\n", port);

  for (;;) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof peer;
    netlab::Fd client(::accept(listener.get(), reinterpret_cast<sockaddr*>(&peer), &peer_len));
    if (!client.valid()) {
      if (errno == EINTR || errno == ECONNABORTED) continue;  // a client gave up while queued
      std::perror("accept");
      return 2;
    }
    char ip[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
    std::fprintf(stderr, "client %s:%u connected\n", ip, ntohs(peer.sin_port));
    serve(client.get());
    std::fprintf(stderr, "client %s:%u disconnected\n", ip, ntohs(peer.sin_port));
  }  // client's destructor closes the socket here, sending our FIN
}
