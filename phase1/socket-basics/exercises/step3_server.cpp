// Step 3: an iterative TCP server that sends back everything it receives, uppercased.
//
//   step3_server PORT
//
// Listen on 0.0.0.0:PORT. For each client, in turn: receive, uppercase, send
// back, until the client closes; then close it and accept the next one.
//
// Build: cd phase1 && cmake --build build      Check: python3 socket-basics/check.py 3
// Try:   ./build/socket-basics/step3_server 9000   and in another terminal:  nc 127.0.0.1 9000
// man pages: socket(2), setsockopt(2), bind(2), listen(2), accept(2), signal(3)

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
  // TODO 1: the same loop as step 2 (copy yours over).
  (void)fd;
  (void)data;
  (void)len;
  errno = ENOSYS;
  return false;
}

// Serves one client until it disconnects. Errors end this client, never the server.
void serve(int fd) {
  // TODO 2: loop: n = recv(fd, buf, sizeof buf, 0).
  //           n == 0  → the client closed: return.
  //           n < 0   → retry on EINTR, otherwise return (e.g. ECONNRESET).
  //           n > 0   → uppercase the n bytes (std::toupper on each), send_all them back;
  //                     if that fails, return.
  char buf[4096];
  (void)buf;
  if (!send_all(fd, "step 3 is not written yet\n", 26)) return;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s PORT\n", argv[0]);
    return 1;
  }
  int port = std::atoi(argv[1]);

  // TODO 3: writing to a connection the peer has already reset can make the kernel send
  //         SIGPIPE, which kills the whole process. Ignore it, std::signal(SIGPIPE, SIG_IGN),
  //         so send() returns -1 with errno EPIPE instead. (The README explains when it fires.)

  // TODO 4: create a TCP socket: socket(AF_INET, SOCK_STREAM, 0), wrapped in netlab::Fd.

  // TODO 5: setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) with int one = 1.
  //         (Also try leaving it out first, and see what the restart test says.)

  // TODO 6: fill a sockaddr_in: sin_family = AF_INET, sin_addr.s_addr = htonl(INADDR_ANY),
  //         sin_port = htons(port). htons/htonl: the network wants big-endian numbers.
  //         Then bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr).

  // TODO 7: listen(fd, 16). 16 is the size of the queue of finished handshakes
  //         waiting for accept().

  // TODO 8: loop forever: accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_len)
  //         into a netlab::Fd (retry on EINTR or ECONNABORTED), print the client's address
  //         with inet_ntop + ntohs(peer.sin_port), call serve(), and let the Fd close it.
  std::fprintf(stderr, "step 3: not written yet (port %d)\n", port);
  serve(-1);
  return 2;
}
