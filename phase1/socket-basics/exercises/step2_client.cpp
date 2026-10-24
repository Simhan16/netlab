// Step 2: the step 1 client again, on the raw sockets API.
//
//   step2_client HOST PORT [PATH]
//
// Same behavior and exit codes as step 1: 0 ok, 1 usage, 2 resolve/connect/IO
// error with a message on stderr. This time you do by hand what Python's
// create_connection() and sendall() did for you.
//
// Build: cd phase1 && cmake --build build      Check: python3 socket-basics/check.py 2
// Try:   ./build/socket-basics/step2_client example.com 80
// man pages to keep open: getaddrinfo(3), socket(2), connect(2), send(2), recv(2)

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "netlab/fd.hpp"  // netlab::Fd closes the socket for you when it goes out of scope

namespace {

// Returns a connected socket, or an invalid Fd after printing why to stderr.
netlab::Fd connect_to(const char* host, const char* port) {
  // TODO 1: call getaddrinfo(host, port, &hints, &results) with hints.ai_family =
  //         AF_UNSPEC (IPv4 or IPv6) and hints.ai_socktype = SOCK_STREAM (TCP).
  //         On failure it returns a nonzero code: print gai_strerror(code).
  // TODO 2: walk the linked list (ai = results; ai; ai = ai->ai_next). For each one:
  //         socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol), then
  //         connect(fd, ai->ai_addr, ai->ai_addrlen). Keep the first that connects.
  //         Why every address? "localhost" usually gives ::1 and 127.0.0.1, and a
  //         server may listen on only one of them.
  // TODO 3: freeaddrinfo(results), and if nothing connected, print strerror(errno)
  //         from the last failure.
  std::fprintf(stderr, "connect_to(%s, %s): not written yet\n", host, port);
  std::exit(3);  // delete this line once connect_to works
}

// Sends all len bytes, or returns false (errno says why).
bool send_all(int fd, const char* data, size_t len) {
  // TODO 4: send() returns how many bytes it accepted, which can be fewer than len.
  //         Loop until everything is sent. If it returns -1 with errno == EINTR
  //         (interrupted by a signal), just try again.
  (void)fd;
  (void)data;
  (void)len;
  errno = ENOSYS;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr, "usage: %s HOST PORT [PATH]\n", argv[0]);
    return 1;
  }
  const char* host = argv[1];
  const char* path = argc == 4 ? argv[3] : "/";

  netlab::Fd sock = connect_to(host, argv[2]);
  if (!sock.valid()) return 2;

  std::string request = std::string("GET ") + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
  if (!send_all(sock.get(), request.data(), request.size())) {
    std::fprintf(stderr, "send: %s\n", std::strerror(errno));
    return 2;
  }

  // TODO 5: loop on recv(sock.get(), buf, sizeof buf, 0) and fwrite each chunk to stdout.
  //         0 means the server closed the connection: you're done, return 0.
  //         -1 is an error (retry on EINTR; otherwise print it and return 2).
  return 2;
}
