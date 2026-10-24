// Step 6 solution: a line-based chat server for many clients at once, with poll().
//
//   step6_chat_server PORT
//
// Every complete line (ending in '\n') a client sends goes to every *other*
// connected client, unchanged. One thread, no blocking reads: poll() reports
// which sockets have something to read, and we only call recv() on those.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "netlab/fd.hpp"

namespace {

constexpr size_t kMaxLine = 4096;

struct Client {
  netlab::Fd fd;
  std::string partial;  // bytes received after the last '\n': the start of a line
};

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
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s PORT\n", argv[0]);
    return 1;
  }
  std::signal(SIGPIPE, SIG_IGN);

  netlab::Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
  int one = 1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(std::atoi(argv[1])));
  if (!listener.valid() || ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
      ::bind(listener.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 ||
      ::listen(listener.get(), 128) < 0) {
    std::perror("listen setup");
    return 2;
  }
  std::fprintf(stderr, "chat listening on 0.0.0.0:%s\n", argv[1]);

  std::vector<Client> clients;
  for (;;) {
    // Rebuild the poll list each round: slot 0 is the listener, slot i+1 is clients[i].
    std::vector<pollfd> fds;
    fds.push_back({listener.get(), POLLIN, 0});
    for (const Client& c : clients) fds.push_back({c.fd.get(), POLLIN, 0});

    if (::poll(fds.data(), fds.size(), -1) < 0) {  // -1: wait as long as it takes
      if (errno == EINTR) continue;
      std::perror("poll");
      return 2;
    }

    std::vector<bool> drop(clients.size(), false);
    for (size_t i = 0; i < clients.size(); ++i) {
      // POLLHUP/POLLERR also mean "recv() will tell you what happened".
      if (!(fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      char buf[4096];
      ssize_t n = ::recv(clients[i].fd.get(), buf, sizeof buf, 0);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {  // 0: the client closed; < 0: ECONNRESET and friends
        drop[i] = true;
        continue;
      }
      clients[i].partial.append(buf, static_cast<size_t>(n));

      // Hand out every complete line; keep the unfinished tail for next time.
      size_t start = 0;
      for (size_t nl; (nl = clients[i].partial.find('\n', start)) != std::string::npos; start = nl + 1) {
        const char* line = clients[i].partial.data() + start;
        size_t len = nl + 1 - start;
        for (size_t j = 0; j < clients.size(); ++j) {
          if (j == i || drop[j]) continue;
          // A blocking send to a client that never reads would stall everyone.
          // echo-server's epoll version fixes that with per-client output buffers.
          if (!send_all(clients[j].fd.get(), line, len)) drop[j] = true;
        }
      }
      clients[i].partial.erase(0, start);
      if (clients[i].partial.size() > kMaxLine) drop[i] = true;  // a "line" that never ends: protect our memory
    }

    // Remove dropped clients (each Fd closes as its Client is destroyed).
    std::vector<Client> kept;
    for (size_t i = 0; i < clients.size(); ++i)
      if (!drop[i]) kept.push_back(std::move(clients[i]));
    clients = std::move(kept);

    // Accept last, so this round's indexes into fds[] stayed valid above.
    if (fds[0].revents & POLLIN) {
      netlab::Fd fd(::accept(listener.get(), nullptr, nullptr));
      if (fd.valid()) clients.push_back({std::move(fd), {}});
    }
  }
}
