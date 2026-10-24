// Step 5 solution: a UDP server that replies to each datagram with the same
// bytes uppercased.
//
//   step5_udp_server PORT
//
// socket(SOCK_DGRAM) → bind → loop { recvfrom → sendto the same address }.
// No listen, no accept, no connection: one socket serves every client, and
// recvfrom() tells you who each datagram came from.

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
  netlab::Fd sock(::socket(AF_INET, SOCK_DGRAM, 0));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(std::atoi(argv[1])));
  if (!sock.valid() || ::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    std::perror("bind");
    return 2;
  }
  std::fprintf(stderr, "listening on udp 0.0.0.0:%s\n", argv[1]);

  // One recvfrom() returns exactly one datagram. If the buffer is smaller than
  // the datagram, the rest is silently thrown away, so size it for the largest
  // possible UDP payload (65,507 bytes over IPv4).
  std::vector<char> buf(65536);
  for (;;) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof peer;
    ssize_t n = ::recvfrom(sock.get(), buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (n < 0) {
      if (errno == EINTR) continue;
      // An ICMP error caused by an earlier sendto can show up here (e.g.
      // ECONNREFUSED on Linux). It concerns one old client, so keep serving.
      std::fprintf(stderr, "recvfrom: %s\n", std::strerror(errno));
      continue;
    }
    // n == 0 is a valid, empty datagram, not end-of-file: UDP has no connection to close.
    for (ssize_t i = 0; i < n; ++i) buf[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(buf[i])));
    if (::sendto(sock.get(), buf.data(), static_cast<size_t>(n), 0, reinterpret_cast<sockaddr*>(&peer), peer_len) < 0) {
      std::fprintf(stderr, "sendto: %s\n", std::strerror(errno));  // a datagram is all-or-nothing: no partial sends
    }
  }
}
