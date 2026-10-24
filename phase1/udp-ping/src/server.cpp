// udp_pong: echoes valid probes back to whoever sent them.
//   udp_pong [port]   (default 9999)

#include <sys/socket.h>

#include <cstdio>
#include <exception>

#include "netlab/socket.hpp"
#include "protocol.hpp"

int main(int argc, char** argv) {
  try {
    uint16_t port = argc > 1 ? netlab::parse_port(argv[1]) : 9999;
    netlab::Fd fd = netlab::udp_bind(port);
    std::printf("udp_pong listening on :%u\n", port);

    uint8_t buf[65535];
    unsigned long long echoed = 0;
    for (;;) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof peer;
      // UDP has no connections: each recvfrom tells us who this datagram came from.
      ssize_t n = ::recvfrom(fd.get(), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
      if (n < 0) {
        if (errno == EINTR) continue;
        netlab::throw_errno("recvfrom");
      }
      udpping::Probe probe;
      if (!udpping::decode(buf, static_cast<size_t>(n), probe)) continue;  // not ours; don't reflect junk
      if (::sendto(fd.get(), buf, static_cast<size_t>(n), 0, reinterpret_cast<sockaddr*>(&peer), peer_len) < 0) {
        std::perror("sendto");
      }
      if (++echoed % 1000 == 0) std::printf("echoed %llu probes\n", echoed);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
