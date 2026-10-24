// TCP echo client.
//
//   echo_client [host] [port]                       interactive: echo stdin lines
//   echo_client [host] [port] --bench N [--size S] [--conns C]
//
// Bench mode opens C connections up front (so the server must juggle them all)
// and does N request/response round trips spread across them, reporting
// throughput and latency. Each round trip waits for its full echo, so this
// measures latency-bound ping-pong, not raw bandwidth.

#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "netlab/socket.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// TCP is a byte stream, not messages: one send() can arrive as several recv()s.
// So "read the echo" means "keep reading until we have len bytes".
bool recv_exact(int fd, char* buf, size_t len) {
  while (len > 0) {
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n == 0) return false;
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    buf += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

int interactive(const std::string& host, uint16_t port) {
  netlab::Fd fd = netlab::tcp_connect(host, port);
  std::fprintf(stderr, "connected to %s:%u, type lines (Ctrl-D to quit)\n", host.c_str(), port);
  std::string line;
  while (std::getline(std::cin, line)) {
    line += '\n';
    std::string echo(line.size(), '\0');
    if (!netlab::send_all(fd.get(), line.data(), line.size()) ||
        !recv_exact(fd.get(), echo.data(), echo.size())) {
      std::fprintf(stderr, "connection lost\n");
      return 1;
    }
    std::cout << echo << std::flush;
  }
  ::shutdown(fd.get(), SHUT_WR);  // send FIN; destructor then closes
  return 0;
}

int bench(const std::string& host, uint16_t port, size_t rounds, size_t size, size_t conns) {
  std::vector<netlab::Fd> sockets;
  sockets.reserve(conns);
  for (size_t i = 0; i < conns; ++i) sockets.push_back(netlab::tcp_connect(host, port));

  std::string msg(size, 'x');
  std::string echo(size, '\0');
  std::vector<double> rtts_us;
  rtts_us.reserve(rounds);

  auto start = Clock::now();
  for (size_t i = 0; i < rounds; ++i) {
    int fd = sockets[i % conns].get();
    auto t0 = Clock::now();
    if (!netlab::send_all(fd, msg.data(), msg.size()) || !recv_exact(fd, echo.data(), echo.size())) {
      std::fprintf(stderr, "connection lost after %zu round trips\n", i);
      return 1;
    }
    rtts_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
  }
  double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

  std::sort(rtts_us.begin(), rtts_us.end());
  auto pct = [&](double p) { return rtts_us[static_cast<size_t>(p * static_cast<double>(rtts_us.size() - 1))]; };
  std::printf("%zu round trips of %zu bytes over %zu connections in %.3f s\n", rounds, size, conns, elapsed);
  std::printf("throughput: %.0f msgs/s, %.2f MB/s echoed\n", static_cast<double>(rounds) / elapsed,
              static_cast<double>(rounds * size) / elapsed / 1e6);
  std::printf("latency us: p50 %.1f  p99 %.1f  max %.1f\n", pct(0.50), pct(0.99), rtts_us.back());
  return 0;
}

size_t parse_count(const char* flag, const char* value) {
  if (value == nullptr) throw std::invalid_argument(std::string(flag) + " needs a value");
  size_t used = 0;
  unsigned long long v = std::stoull(value, &used);
  if (value[used] != '\0' || v == 0) throw std::invalid_argument(std::string("bad value for ") + flag);
  return static_cast<size_t>(v);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    size_t rounds = 0, size = 64, conns = 1;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
      if (arg == "--bench") {
        rounds = parse_count("--bench", next), ++i;
      } else if (arg == "--size") {
        size = parse_count("--size", next), ++i;
      } else if (arg == "--conns") {
        conns = parse_count("--conns", next), ++i;
      } else if (arg == "-h" || arg == "--help") {
        std::puts("usage: echo_client [host] [port] [--bench N [--size S] [--conns C]]");
        return 0;
      } else if (positional == 0) {
        host = arg, ++positional;
      } else if (positional == 1) {
        port = netlab::parse_port(arg), ++positional;
      } else {
        throw std::invalid_argument("unexpected argument: " + arg);
      }
    }
    netlab::ignore_sigpipe();
    return rounds > 0 ? bench(host, port, rounds, size, conns) : interactive(host, port);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
