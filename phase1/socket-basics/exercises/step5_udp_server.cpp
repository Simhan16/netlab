// Step 5: a UDP server that replies to each datagram with the same bytes uppercased.
//
//   step5_udp_server PORT
//
// Bind a UDP socket to 0.0.0.0:PORT. Loop: receive one datagram, uppercase it,
// and send it back to whoever sent it. Keep serving through errors.
//
// Build: cd phase1 && cmake --build build      Check: python3 socket-basics/check.py 5
// Try:   ./build/socket-basics/step5_udp_server 9001   and:  nc -u 127.0.0.1 9001
// man pages: recvfrom(2), sendto(2)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "netlab/fd.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s PORT\n", argv[0]);
    return 1;
  }

  // TODO 1: socket(AF_INET, SOCK_DGRAM, 0), then bind it to INADDR_ANY:PORT as in step 3.
  //         No listen() and no accept(): UDP has no connections.

  // TODO 2: allocate a receive buffer. How big? A datagram larger than the buffer is
  //         silently cut short, so think about the largest UDP payload (65,507 bytes).

  // TODO 3: loop:
  //   a. n = recvfrom(fd, buf, size, 0, reinterpret_cast<sockaddr*>(&peer), &peer_len),
  //      with sockaddr_in peer and socklen_t peer_len = sizeof peer set fresh each time.
  //   b. n < 0: print the error and continue (EINTR, or an ICMP error from an earlier send).
  //      n == 0 is NOT end-of-file here. It's a real, empty datagram: reply with an empty one.
  //   c. uppercase n bytes and sendto(fd, buf, n, 0, (sockaddr*)&peer, peer_len).
  std::fprintf(stderr, "step 5: not written yet (port %s)\n", argv[1]);
  return 2;
}
