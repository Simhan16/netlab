// Step 4: a length-prefixed message server.
//
//   step4_framed_server PORT
//
// Protocol, both directions: a 4-byte big-endian length N, then N bytes of payload.
// Reply to each message with one message: the same payload, uppercased.
// Rules:
//   - 0 <= N <= 1 MiB (1048576). A bigger N is a protocol error: close that
//     client's connection (and keep serving others).
//   - A client that closes part-way through a message is an error too: close it.
//   - A client that closes between messages is the normal end.
//
// Start from your step 3 server: listen setup, accept loop and send_all are the same.
//
// Build: cd phase1 && cmake --build build      Check: python3 socket-basics/check.py 4
// man pages: recv(2), ntohl(3)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "netlab/fd.hpp"

namespace {

constexpr uint32_t kMaxMessage = 1 << 20;  // 1 MiB

enum class ReadResult { kOk, kEof, kError };

// Reads exactly len bytes into data, however many recv() calls that takes.
// Returns kEof if the peer closed before the first byte, kError if it closed
// part-way (or recv failed), kOk when all len bytes are in.
ReadResult recv_exact(int fd, char* data, size_t len) {
  // TODO 1: keep a count of bytes received. Loop until it reaches len, calling
  //         recv(fd, data + got, len - got, 0). Handle 0, -1 and EINTR as described above.
  (void)fd;
  (void)data;
  (void)len;
  return ReadResult::kError;
}

bool send_all(int fd, const char* data, size_t len) {
  // TODO 2: copy from step 3.
  (void)fd;
  (void)data;
  (void)len;
  errno = ENOSYS;
  return false;
}

void serve(int fd) {
  // TODO 3: loop:
  //   a. recv_exact 4 bytes into a uint32_t. kEof → return quietly; kError → return.
  //   b. len = ntohl(that value). If len > kMaxMessage, return (that drops the client).
  //      Never trust a length from the network.
  //   c. resize a std::vector<char> to len and recv_exact the payload into it (skip when len == 0).
  //   d. uppercase it, then send_all htonl(len) (4 bytes), then send_all the payload.
  std::vector<char> payload;
  if (recv_exact(fd, payload.data(), 0) != ReadResult::kOk) return;
  if (!send_all(fd, payload.data(), payload.size())) return;
  (void)kMaxMessage;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s PORT\n", argv[0]);
    return 1;
  }
  // TODO 4: the same setup and accept loop as step 3, calling serve() for each client.
  std::fprintf(stderr, "step 4: not written yet (port %s)\n", argv[1]);
  serve(-1);
  return 2;
}
