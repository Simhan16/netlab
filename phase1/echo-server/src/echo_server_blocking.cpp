// Iterative TCP echo server: serves exactly one client at a time.
//
// This is step one on purpose. Connect two clients at once: the second one's
// handshake completes (the kernel does that, into the listen backlog), but it
// gets no echo until the first disconnects, because this thread is stuck in
// recv() for client #1. echo_server_epoll fixes that.

#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <exception>

#include "netlab/socket.hpp"

int main(int argc, char** argv) {
  try {
    uint16_t port = argc > 1 ? netlab::parse_port(argv[1]) : 9000;
    netlab::ignore_sigpipe();
    netlab::Fd listener = netlab::tcp_listen(port);
    std::printf("echo_server_blocking listening on :%u (one client at a time)\n", port);

    for (;;) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof peer;
      netlab::Fd client(::accept(listener.get(), reinterpret_cast<sockaddr*>(&peer), &peer_len));
      if (!client.valid()) {
        if (errno == EINTR) continue;
        netlab::throw_errno("accept");
      }
      std::string who = netlab::to_string(peer);
      std::printf("+ %s connected\n", who.c_str());

      char buf[4096];
      size_t total = 0;
      for (;;) {
        ssize_t n = ::recv(client.get(), buf, sizeof buf, 0);
        if (n == 0) break;  // peer sent FIN: orderly shutdown
        if (n < 0) {
          if (errno == EINTR) continue;
          std::perror("recv");
          break;
        }
        if (!netlab::send_all(client.get(), buf, static_cast<size_t>(n))) {
          std::perror("send");
          break;
        }
        total += static_cast<size_t>(n);
      }
      std::printf("- %s disconnected after %zu bytes\n", who.c_str(), total);
      // client's destructor closes the socket, which sends our FIN.
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
