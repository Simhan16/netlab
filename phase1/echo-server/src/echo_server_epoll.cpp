// Event-driven TCP echo server: one thread, many clients, via epoll (Linux).
//
// The model: every socket is non-blocking, and epoll tells us which ones are
// ready. We never wait on a single client, so a slow client can't stall others.
//
// Two details that separate a toy from a real server:
//  * Partial writes. send() on a non-blocking socket may accept only part of
//    the data (the kernel send buffer is full). The rest goes into a per-client
//    output buffer, and we ask epoll for EPOLLOUT ("tell me when writable").
//  * Backpressure. If a client sends faster than it reads its echoes, its
//    output buffer grows without bound. Past kMaxPending we stop reading from
//    it (drop EPOLLIN) until it drains. TCP flow control then pushes back on
//    the sender: its window shrinks to zero. You can watch that in tcpdump.

#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <unordered_map>

#include "netlab/socket.hpp"

namespace {

constexpr size_t kMaxPending = 1 << 20;  // 1 MiB of un-echoed data per client
constexpr int kMaxEvents = 256;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Connection {
  netlab::Fd fd;
  std::string peer;
  std::string pending;  // bytes received but not yet echoed back
  bool reading = true;
  bool peer_closed = false;  // got FIN; close once pending is flushed
};

struct Stats {
  size_t accepted = 0;
  size_t peak_clients = 0;
  size_t bytes_echoed = 0;
};

class EchoServer {
 public:
  explicit EchoServer(uint16_t port)
      : listener_(netlab::tcp_listen(port, 4096)), epoll_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (!epoll_.valid()) netlab::throw_errno("epoll_create1");
    netlab::set_nonblocking(listener_.get());
    watch(listener_.get(), EPOLLIN, EPOLL_CTL_ADD);
  }

  void run() {
    epoll_event events[kMaxEvents];
    while (!g_stop) {
      int n = ::epoll_wait(epoll_.get(), events, kMaxEvents, -1);
      if (n < 0) {
        if (errno == EINTR) continue;  // a signal arrived; loop re-checks g_stop
        netlab::throw_errno("epoll_wait");
      }
      for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        if (fd == listener_.get()) {
          accept_all();
        } else {
          handle(fd, events[i].events);
        }
      }
    }
  }

  const Stats& stats() const { return stats_; }

 private:
  void watch(int fd, uint32_t events, int op) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_.get(), op, fd, &ev) < 0) netlab::throw_errno("epoll_ctl");
  }

  void update_interest(Connection& c) {
    uint32_t events = 0;
    if (c.reading) events |= EPOLLIN;
    if (!c.pending.empty()) events |= EPOLLOUT;
    watch(c.fd.get(), events, EPOLL_CTL_MOD);
  }

  void accept_all() {
    // Several clients may be waiting; drain the accept queue until EAGAIN.
    for (;;) {
      sockaddr_storage peer{};
      socklen_t len = sizeof peer;
      int fd = ::accept(listener_.get(), reinterpret_cast<sockaddr*>(&peer), &len);
      if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR || errno == ECONNABORTED) continue;
        // EMFILE (out of fds) is the classic overload failure; log and carry on.
        std::perror("accept");
        return;
      }
      netlab::Fd owned(fd);
      netlab::set_nonblocking(fd);
      watch(fd, EPOLLIN, EPOLL_CTL_ADD);
      conns_.emplace(fd, Connection{std::move(owned), netlab::to_string(peer)});
      ++stats_.accepted;
      stats_.peak_clients = std::max(stats_.peak_clients, conns_.size());
    }
  }

  void handle(int fd, uint32_t events) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Connection& c = it->second;

    bool alive = true;
    if (events & (EPOLLERR | EPOLLHUP)) alive = false;
    if (alive && (events & EPOLLIN)) alive = read_from(c);
    if (alive && (events & EPOLLOUT)) alive = flush(c);

    if (!alive || (c.peer_closed && c.pending.empty())) {
      conns_.erase(it);  // Fd destructor closes; close also removes it from epoll
      return;
    }
    c.reading = !c.peer_closed && c.pending.size() < kMaxPending;
    update_interest(c);
  }

  // Returns false when the connection should be closed.
  bool read_from(Connection& c) {
    char buf[16384];
    while (!c.peer_closed && c.pending.size() < kMaxPending) {
      ssize_t n = ::recv(c.fd.get(), buf, sizeof buf, 0);
      if (n == 0) {  // FIN: the client is done sending, but may still be reading
        c.peer_closed = true;
        break;
      }
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        return false;  // ECONNRESET etc.
      }
      c.pending.append(buf, static_cast<size_t>(n));
    }
    return flush(c);
  }

  bool flush(Connection& c) {
    while (!c.pending.empty()) {
      ssize_t n = ::send(c.fd.get(), c.pending.data(), c.pending.size(), 0);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // wait for EPOLLOUT
        if (errno == EINTR) continue;
        return false;
      }
      c.pending.erase(0, static_cast<size_t>(n));
      stats_.bytes_echoed += static_cast<size_t>(n);
    }
    return true;
  }

  netlab::Fd listener_;
  netlab::Fd epoll_;
  std::unordered_map<int, Connection> conns_;
  Stats stats_;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    uint16_t port = argc > 1 ? netlab::parse_port(argv[1]) : 9000;
    netlab::ignore_sigpipe();

    struct sigaction sa{};
    sa.sa_handler = on_signal;  // no SA_RESTART: we want epoll_wait to return EINTR
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    EchoServer server(port);
    std::printf("echo_server_epoll listening on :%u (Ctrl-C for stats)\n", port);
    server.run();

    const Stats& s = server.stats();
    std::printf("\naccepted %zu connections, peak %zu concurrent, echoed %zu bytes\n", s.accepted,
                s.peak_clients, s.bytes_echoed);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
