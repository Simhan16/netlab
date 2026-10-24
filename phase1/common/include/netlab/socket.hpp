#pragma once

#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

#include "netlab/fd.hpp"

namespace netlab {

[[noreturn]] inline void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// Parses a TCP/UDP port number, throwing std::invalid_argument on garbage.
uint16_t parse_port(const std::string& text);

// Writing to a socket whose peer has gone away raises SIGPIPE, which kills the
// process by default. Servers ignore it and handle the EPIPE error instead.
void ignore_sigpipe();

void set_nonblocking(int fd);

// Dual-stack listening socket on all addresses (IPv6 socket accepting IPv4 too).
Fd tcp_listen(uint16_t port, int backlog = 128);
Fd udp_bind(uint16_t port);

// Resolves host with getaddrinfo and connects to the first address that works.
// For UDP, "connect" only fixes the default peer: no packets are exchanged.
Fd tcp_connect(const std::string& host, uint16_t port);
Fd udp_connect(const std::string& host, uint16_t port);

// send() may write fewer bytes than asked; this loops until done.
// Returns false on error (errno set).
bool send_all(int fd, const void* data, size_t len);

// "192.0.2.1:53" or "[2001:db8::1]:53"; IPv4-mapped IPv6 shown as plain IPv4.
std::string to_string(const sockaddr_storage& addr);

}  // namespace netlab
