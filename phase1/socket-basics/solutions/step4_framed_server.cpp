// Step 4 solution: a length-prefixed message server.
//
//   step4_framed_server PORT
//
// Protocol, both directions: a 4-byte big-endian length N, then N bytes of
// payload. Each reply carries the request's payload uppercased. N > 1 MiB is a
// protocol error: the server drops that client (and only that client).
//
// TCP is a byte stream. It keeps bytes in order but not message boundaries: one
// send() of 100 bytes may arrive as 100 recv()s of 1 byte, and three sends may
// arrive in one recv(). recv_exact() is how you put the boundaries back.

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

// Reads exactly len bytes, however many recv() calls that takes.
// kEof only if the peer closed before sending the first byte: a clean end
// between messages. A close part-way through is an error (truncated message).
ReadResult recv_exact(int fd, char* data, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = ::recv(fd, data + got, len - got, 0);
    if (n == 0) return got == 0 ? ReadResult::kEof : ReadResult::kError;
    if (n < 0) {
      if (errno == EINTR) continue;
      return ReadResult::kError;
    }
    got += static_cast<size_t>(n);
  }
  return ReadResult::kOk;
}

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

void serve(int fd) {
  std::vector<char> payload;
  for (;;) {
    uint32_t wire_len = 0;
    ReadResult r = recv_exact(fd, reinterpret_cast<char*>(&wire_len), sizeof wire_len);
    if (r == ReadResult::kEof) return;
    if (r == ReadResult::kError) {
      std::fprintf(stderr, "  truncated header or recv error\n");
      return;
    }
    uint32_t len = ntohl(wire_len);  // the wire is big-endian; this CPU probably isn't
    // Never trust a length from the network: without this check, a client could
    // make us allocate 4 GiB with four bytes.
    if (len > kMaxMessage) {
      std::fprintf(stderr, "  message of %u bytes exceeds the %u-byte limit, dropping client\n", len, kMaxMessage);
      return;
    }
    payload.resize(len);
    if (len > 0 && recv_exact(fd, payload.data(), len) != ReadResult::kOk) {
      std::fprintf(stderr, "  truncated message (wanted %u bytes)\n", len);
      return;
    }
    for (char& c : payload) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    uint32_t reply_len = htonl(len);
    if (!send_all(fd, reinterpret_cast<const char*>(&reply_len), sizeof reply_len) ||
        !send_all(fd, payload.data(), payload.size())) {
      std::fprintf(stderr, "  send: %s\n", std::strerror(errno));
      return;
    }
  }
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
      ::listen(listener.get(), 16) < 0) {
    std::perror("listen setup");
    return 2;
  }
  std::fprintf(stderr, "listening on 0.0.0.0:%s\n", argv[1]);

  for (;;) {
    netlab::Fd client(::accept(listener.get(), nullptr, nullptr));
    if (!client.valid()) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      std::perror("accept");
      return 2;
    }
    serve(client.get());
  }
}
