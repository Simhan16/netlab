// Step 6: a line-based chat server for many clients at once, using poll().
//
//   step6_chat_server PORT
//
// Every complete line (ending in '\n') a client sends goes to every *other*
// connected client, unchanged. Rules:
//   - A line may arrive in several pieces, and one recv() may hold several lines:
//     keep a per-client buffer and only forward complete lines.
//   - A client that disconnects (cleanly or by reset) is removed; everyone else carries on.
//   - A client whose unfinished line grows past 4096 bytes is disconnected.
//
// Build: cd phase1 && cmake --build build      Check: python3 socket-basics/check.py 6
// Try:   ./build/socket-basics/step6_chat_server 9002   then  nc 127.0.0.1 9002  in two or three terminals
// man pages: poll(2)

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
  std::string partial;  // bytes received after the last '\n'
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s PORT\n", argv[0]);
    return 1;
  }
  // TODO 1: ignore SIGPIPE and set up the listening socket, as in step 3
  //         (use a bigger listen backlog, like 128).

  std::vector<Client> clients;
  (void)kMaxLine;

  // TODO 2: the event loop. Each round:
  //   a. Build a std::vector<pollfd>: {listener, POLLIN, 0} first, then
  //      {client.fd.get(), POLLIN, 0} for each client.
  //   b. poll(fds.data(), fds.size(), -1) sleeps until at least one socket is ready.
  //   c. For each client whose revents has POLLIN, POLLHUP or POLLERR: recv() once
  //      (it won't block now). n <= 0 → mark the client to be removed. Otherwise
  //      append to its `partial`, then forward each complete line to every other
  //      client and erase it from `partial`.
  //   d. Remove marked clients only after the loop over them. Erasing from a vector
  //      while you iterate over it (and over fds) is a classic bug.
  //   e. If the listener has POLLIN, accept() and push_back a new Client.
  std::fprintf(stderr, "step 6: not written yet (port %s, %zu clients)\n", argv[1], clients.size());
  return 2;
}
