#pragma once

// DNS wire format (RFC 1035): build queries, parse responses. No sockets here,
// so the codec is reusable by mini-tcpdump and testable without a network.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dns {

namespace type {
constexpr uint16_t A = 1, NS = 2, CNAME = 5, SOA = 6, PTR = 12, MX = 15, TXT = 16, AAAA = 28, ANY = 255;
}
constexpr uint16_t kClassIN = 1;

namespace rcode {
constexpr uint8_t NOERROR = 0, FORMERR = 1, SERVFAIL = 2, NXDOMAIN = 3, NOTIMP = 4, REFUSED = 5;
}

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The 12-byte header. `flags` packs QR|Opcode|AA|TC|RD|RA|Z|RCODE.
struct Header {
  uint16_t id = 0;
  uint16_t flags = 0;
  uint16_t qdcount = 0, ancount = 0, nscount = 0, arcount = 0;

  bool is_response() const { return flags & 0x8000; }
  bool authoritative() const { return flags & 0x0400; }
  bool truncated() const { return flags & 0x0200; }  // TC: answer didn't fit in UDP; retry over TCP
  bool recursion_desired() const { return flags & 0x0100; }
  bool recursion_available() const { return flags & 0x0080; }
  uint8_t rcode() const { return flags & 0x000F; }
};

struct Question {
  std::string name;
  uint16_t qtype = 0;
  uint16_t qclass = kClassIN;
};

struct Record {
  std::string name;
  uint16_t rtype = 0;
  uint16_t rclass = kClassIN;
  uint32_t ttl = 0;
  std::string data;              // presentation format, e.g. "93.184.216.34" or "10 mx.example."
  std::string target;            // for NS/CNAME/PTR/MX: the domain name in the RDATA
};

struct Message {
  Header header;
  std::vector<Question> questions;
  std::vector<Record> answers, authority, additional;
};

std::optional<uint16_t> type_from_string(std::string_view text);
std::string type_to_string(uint16_t t);
std::string rcode_to_string(uint8_t rc);

// Throws std::invalid_argument for names that can't be encoded (label > 63 bytes, etc).
std::vector<uint8_t> build_query(uint16_t id, std::string_view name, uint16_t qtype, bool recursion_desired = true);

// Throws ParseError on malformed or truncated input.
Message parse(const uint8_t* data, size_t len);
inline Message parse(const std::vector<uint8_t>& buf) { return parse(buf.data(), buf.size()); }

// Lowercase, trailing-dot form, for comparing names ("Example.COM" == "example.com.").
std::string canonical(std::string_view name);

}  // namespace dns
