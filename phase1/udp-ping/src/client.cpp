// udp_ping: measures latency, jitter and loss to a udp_pong server.
//
//   udp_ping host [-p port] [-c count] [-i interval_ms] [-s size] [-W timeout_ms]
//
// Unlike ICMP ping this runs as a normal user, goes through the same firewall
// rules and queues as application UDP traffic, and lets you pick the size.

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "netlab/socket.hpp"
#include "protocol.hpp"

namespace {

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

struct Options {
  std::string host;
  uint16_t port = 9999;
  uint32_t count = 10;
  int interval_ms = 1000;
  size_t size = 64;
  int timeout_ms = 1000;
};

struct Stats {
  uint32_t sent = 0;
  uint32_t received = 0;  // unique replies
  uint32_t duplicates = 0;
  uint32_t reordered = 0;  // arrived after a higher sequence number
  uint32_t late = 0;       // arrived after their timeout (counted received, flagged)
  std::vector<double> rtts_ms;
  double jitter_ms = 0;  // RFC 3550 smoothed mean deviation of consecutive transit times
  double last_rtt_ms = -1;
  int64_t highest_seq = -1;
  std::vector<bool> seen;
};

void handle_reply(const Options& o, Stats& s, const uint8_t* buf, size_t len) {
  udpping::Probe p;
  if (!udpping::decode(buf, len, p) || p.seq >= s.sent) return;
  double rtt = static_cast<double>(now_ns() - p.sent_ns) / 1e6;

  if (s.seen[p.seq]) {
    ++s.duplicates;
    std::printf("%zu bytes from %s: seq=%u DUPLICATE\n", len, o.host.c_str(), p.seq);
    return;
  }
  s.seen[p.seq] = true;
  ++s.received;
  s.rtts_ms.push_back(rtt);

  bool reordered = static_cast<int64_t>(p.seq) < s.highest_seq;
  if (reordered) ++s.reordered;
  s.highest_seq = std::max(s.highest_seq, static_cast<int64_t>(p.seq));
  bool late = rtt > o.timeout_ms;
  if (late) ++s.late;

  if (s.last_rtt_ms >= 0) s.jitter_ms += (std::fabs(rtt - s.last_rtt_ms) - s.jitter_ms) / 16.0;
  s.last_rtt_ms = rtt;

  std::printf("%zu bytes from %s: seq=%u time=%.3f ms%s%s\n", len, o.host.c_str(), p.seq, rtt,
              reordered ? " (out of order)" : "", late ? " (late)" : "");
}

void print_summary(const Options& o, const Stats& s) {
  std::printf("\n--- %s udp_ping statistics ---\n", o.host.c_str());
  double loss = s.sent ? 100.0 * (s.sent - s.received) / s.sent : 0;
  std::printf("%u sent, %u received, %.1f%% loss, %u duplicates, %u reordered, %u late\n", s.sent, s.received,
              loss, s.duplicates, s.reordered, s.late);
  if (s.rtts_ms.empty()) return;
  double sum = 0, sq = 0;
  for (double r : s.rtts_ms) sum += r, sq += r * r;
  double n = static_cast<double>(s.rtts_ms.size());
  double avg = sum / n;
  double mdev = std::sqrt(std::max(0.0, sq / n - avg * avg));
  auto [mn, mx] = std::minmax_element(s.rtts_ms.begin(), s.rtts_ms.end());
  std::printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms, jitter %.3f ms\n", *mn, avg, *mx, mdev, s.jitter_ms);
}

int run(const Options& o) {
  netlab::Fd fd = netlab::udp_connect(o.host, o.port);
  Stats s;
  s.seen.assign(o.count, false);
  std::vector<uint8_t> probe(std::max(o.size, udpping::kHeaderSize), 0);
  uint8_t buf[65535];
  std::printf("UDP PING %s:%u: %zu-byte probes\n", o.host.c_str(), o.port, probe.size());

  auto next_send = Clock::now();
  auto deadline = next_send;  // when to stop waiting after the last probe
  while (!g_stop) {
    auto now = Clock::now();
    if (s.sent < o.count && now >= next_send) {
      udpping::encode({s.sent, now_ns()}, probe.data());
      // On a connected UDP socket, an ICMP port-unreachable from an earlier
      // probe surfaces here as ECONNREFUSED: the server isn't running.
      if (::send(fd.get(), probe.data(), probe.size(), 0) < 0 && errno != ECONNREFUSED) {
        netlab::throw_errno("send");
      }
      ++s.sent;
      next_send += std::chrono::milliseconds(o.interval_ms);
      deadline = now + std::chrono::milliseconds(o.timeout_ms);
    }
    if (s.sent == o.count && (now >= deadline || s.received == s.sent)) break;

    auto wake = s.sent < o.count ? std::min(next_send, deadline) : deadline;
    int wait_ms = static_cast<int>(std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(wake - Clock::now()).count()));
    pollfd p{fd.get(), POLLIN, 0};
    int rc = ::poll(&p, 1, wait_ms);
    if (rc < 0 && errno != EINTR) netlab::throw_errno("poll");
    if (rc > 0) {
      ssize_t n = ::recv(fd.get(), buf, sizeof buf, 0);
      if (n >= 0) {
        handle_reply(o, s, buf, static_cast<size_t>(n));
      } else if (errno == ECONNREFUSED) {
        std::printf("ICMP port unreachable: is udp_pong running on %s:%u?\n", o.host.c_str(), o.port);
      }
    }
  }
  print_summary(o, s);
  return s.received > 0 ? 0 : 1;
}

int parse_int(const char* flag, const char* value, int min) {
  if (value == nullptr) throw std::invalid_argument(std::string(flag) + " needs a value");
  size_t used = 0;
  long v = std::stol(value, &used);
  if (value[used] != '\0' || v < min) throw std::invalid_argument(std::string("bad value for ") + flag);
  return static_cast<int>(v);
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "-p") o.port = netlab::parse_port(next ? next : ""), ++i;
    else if (arg == "-c") o.count = static_cast<uint32_t>(parse_int("-c", next, 1)), ++i;
    else if (arg == "-i") o.interval_ms = parse_int("-i", next, 1), ++i;
    else if (arg == "-s") o.size = static_cast<size_t>(parse_int("-s", next, 1)), ++i;
    else if (arg == "-W") o.timeout_ms = parse_int("-W", next, 1), ++i;
    else if (o.host.empty() && arg[0] != '-') o.host = arg;
    else throw std::invalid_argument("unexpected argument: " + arg);
  }
  if (o.host.empty()) {
    throw std::invalid_argument("usage: udp_ping host [-p port] [-c count] [-i ms] [-s size] [-W timeout_ms]");
  }
  if (o.size > 65507) throw std::invalid_argument("-s: max UDP payload over IPv4 is 65507");
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options o = parse_args(argc, argv);
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    ::sigaction(SIGINT, &sa, nullptr);
    return run(o);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "udp_ping: %s\n", e.what());
    return 2;
  }
}
