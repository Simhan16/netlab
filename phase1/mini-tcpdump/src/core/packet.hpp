#pragma once

// Header decoding: raw captured bytes -> a Packet struct. Pure functions, no
// libpcap dependency, so they are unit-testable with hand-built byte arrays.

#include <array>
#include <cstdint>
#include <string>

namespace pkt {

// pcap link-layer header types (what precedes the IP header in each frame).
namespace link {
constexpr int Null = 0;          // BSD loopback: 4-byte host-order address family
constexpr int Ethernet = 1;
constexpr int RawBsd = 12;       // DLT_RAW on Linux/macOS
constexpr int RawOpenBsd = 14;
constexpr int Raw = 101;         // LINKTYPE_RAW as stored in files
constexpr int Loop = 108;        // OpenBSD loopback: network-order family
constexpr int LinuxSll = 113;    // "cooked" capture, used for `-i any`
constexpr int Ipv4 = 228;
constexpr int Ipv6 = 229;
constexpr int LinuxSll2 = 276;
}  // namespace link

std::string link_name(int linktype);

namespace tcpflag {
constexpr uint8_t FIN = 0x01, SYN = 0x02, RST = 0x04, PSH = 0x08, ACK = 0x10, URG = 0x20, ECE = 0x40, CWR = 0x80;
}

struct IpAddr {
  uint8_t family = 0;  // 4 or 6
  std::array<uint8_t, 16> bytes{};

  std::string str() const;
  bool operator==(const IpAddr& o) const { return family == o.family && bytes == o.bytes; }
  bool operator<(const IpAddr& o) const { return family != o.family ? family < o.family : bytes < o.bytes; }
};

struct Tcp {
  uint16_t sport = 0, dport = 0;
  uint32_t seq = 0, ack = 0;
  uint8_t flags = 0;
  uint16_t window = 0;
  // Options are only sent on SYNs (except SACK blocks and timestamps).
  uint16_t mss = 0;
  int wscale = -1;
  bool sack_permitted = false;
  bool has_sack_blocks = false;
  uint32_t payload_len = 0;       // from the IP length fields: what was on the wire
  uint32_t payload_captured = 0;  // what we actually have (snaplen may cut it)
  const uint8_t* payload = nullptr;

  bool has(uint8_t f) const { return flags & f; }
};

struct Udp {
  uint16_t sport = 0, dport = 0;
  uint32_t payload_len = 0;
  uint32_t payload_captured = 0;
  const uint8_t* payload = nullptr;
};

struct Icmp {
  bool v6 = false;
  uint8_t type = 0, code = 0;
  uint32_t mtu = 0;  // for "fragmentation needed" / "packet too big": the next-hop MTU
  uint16_t echo_id = 0, echo_seq = 0;
  // Error messages quote the header of the packet that caused them, which
  // tells us exactly which flow the error belongs to.
  bool has_inner = false;
  IpAddr inner_src, inner_dst;
  uint8_t inner_proto = 0;
  uint16_t inner_sport = 0, inner_dport = 0;

  bool is_error() const;
  bool is_frag_needed() const { return v6 ? type == 2 : (type == 3 && code == 4); }
  bool is_echo_request() const { return v6 ? type == 128 : type == 8; }
  bool is_echo_reply() const { return v6 ? type == 129 : type == 0; }
  std::string name() const;
};

struct Arp {
  uint16_t op = 0;  // 1 request, 2 reply
  IpAddr sender, target;
};

enum class L4 { None, Tcp, Udp, Icmp, Other };

struct Packet {
  double ts = 0;
  uint32_t wire_len = 0, cap_len = 0;

  bool is_arp = false;
  Arp arp;

  bool is_ip = false;
  IpAddr src, dst;
  uint8_t ttl = 0;
  uint8_t proto = 0;
  bool dont_fragment = false;
  bool fragment = false;  // a non-first IPv4/IPv6 fragment: no L4 header inside
  uint32_t ip_len = 0;

  L4 l4 = L4::None;
  Tcp tcp;
  Udp udp;
  Icmp icmp;
};

// Throws netlab::TruncatedError if headers are cut short and
// std::invalid_argument for unsupported link types.
Packet decode(int linktype, double ts, const uint8_t* data, uint32_t caplen, uint32_t wirelen);

// One-line tcpdump-style description.
std::string describe(const Packet& p);

}  // namespace pkt
