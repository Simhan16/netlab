#include "packet.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "netlab/bytes.hpp"

namespace pkt {

namespace {

constexpr uint16_t kEtherIpv4 = 0x0800, kEtherArp = 0x0806, kEtherIpv6 = 0x86DD;
constexpr uint16_t kEtherVlan = 0x8100, kEtherQinQ = 0x88A8;
constexpr uint8_t kProtoIcmp = 1, kProtoTcp = 6, kProtoUdp = 17, kProtoIcmp6 = 58;

IpAddr read_ip(netlab::Reader& r, uint8_t family) {
  IpAddr a;
  a.family = family;
  size_t n = family == 4 ? 4 : 16;
  r.need(n, "ip address");
  std::memcpy(a.bytes.data(), r.cursor(), n);
  r.skip(n);
  return a;
}

void decode_tcp(netlab::Reader& r, uint32_t l4_len, Tcp& t) {
  size_t start = r.pos();
  t.sport = r.u16();
  t.dport = r.u16();
  t.seq = r.u32();
  t.ack = r.u32();
  uint8_t offset_byte = r.u8();
  t.flags = r.u8();
  t.window = r.u16();
  r.skip(4, "tcp checksum/urgent");
  size_t header_len = static_cast<size_t>(offset_byte >> 4) * 4;
  if (header_len < 20) throw netlab::TruncatedError("tcp data offset < 20");

  // Options: kind, length, value. Kinds 0 (end) and 1 (no-op) have no length.
  size_t opt_end = start + header_len;
  r.need(opt_end - r.pos(), "tcp options");
  while (r.pos() < opt_end) {
    uint8_t kind = r.u8();
    if (kind == 0) break;
    if (kind == 1) continue;
    if (r.pos() >= opt_end) break;
    uint8_t len = r.u8();
    if (len < 2 || r.pos() + len - 2 > opt_end) break;  // malformed option: stop parsing options
    size_t value_end = r.pos() + len - 2;
    if (kind == 2 && len == 4) t.mss = r.u16();
    else if (kind == 3 && len == 3) t.wscale = r.u8();
    else if (kind == 4) t.sack_permitted = true;
    else if (kind == 5) t.has_sack_blocks = true;
    r.seek(value_end);
  }
  r.seek(opt_end);

  t.payload_len = l4_len > header_len ? l4_len - static_cast<uint32_t>(header_len) : 0;
  t.payload_captured = std::min<uint32_t>(t.payload_len, static_cast<uint32_t>(r.remaining()));
  t.payload = r.cursor();
}

void decode_udp(netlab::Reader& r, uint32_t l4_len, Udp& u) {
  u.sport = r.u16();
  u.dport = r.u16();
  uint16_t len = r.u16();
  r.skip(2, "udp checksum");
  uint32_t body = len >= 8 ? len - 8u : 0u;
  if (l4_len >= 8) body = std::min(body, l4_len - 8);
  u.payload_len = body;
  u.payload_captured = std::min<uint32_t>(body, static_cast<uint32_t>(r.remaining()));
  u.payload = r.cursor();
}

// The quoted packet inside an ICMP error: its IP header plus at least the
// first 8 bytes of L4 (enough for ports).
void decode_icmp_inner(netlab::Reader& r, Icmp& ic) {
  try {
    r.need(1, "inner ip");
    uint8_t v = r.cursor()[0] >> 4;
    if (v == 4) {
      size_t start = r.pos();
      size_t ihl = static_cast<size_t>(r.u8() & 0x0F) * 4;
      r.skip(8);
      ic.inner_proto = r.u8();
      r.skip(2);
      ic.inner_src = read_ip(r, 4);
      ic.inner_dst = read_ip(r, 4);
      r.seek(start + ihl);
    } else if (v == 6) {
      r.skip(6);
      ic.inner_proto = r.u8();
      r.skip(1);
      ic.inner_src = read_ip(r, 6);
      ic.inner_dst = read_ip(r, 6);
    } else {
      return;
    }
    ic.has_inner = true;
    if (ic.inner_proto == kProtoTcp || ic.inner_proto == kProtoUdp) {
      ic.inner_sport = r.u16();
      ic.inner_dport = r.u16();
    }
  } catch (const netlab::TruncatedError&) {
    // Quoted header cut short: keep what we got.
  }
}

void decode_icmp(netlab::Reader& r, bool v6, Icmp& ic) {
  ic.v6 = v6;
  ic.type = r.u8();
  ic.code = r.u8();
  r.skip(2, "icmp checksum");
  uint16_t a = r.u16();
  uint16_t b = r.u16();
  if (ic.is_echo_request() || ic.is_echo_reply()) {
    ic.echo_id = a;
    ic.echo_seq = b;
  } else if (!v6 && ic.type == 3 && ic.code == 4) {
    ic.mtu = b;  // RFC 1191: low 16 bits of the second word
  } else if (v6 && ic.type == 2) {
    ic.mtu = static_cast<uint32_t>(a) << 16 | b;
  }
  if (ic.is_error()) decode_icmp_inner(r, ic);
}

void decode_l4(netlab::Reader& r, Packet& p, uint32_t l4_len) {
  if (p.fragment) {
    p.l4 = L4::Other;
    return;
  }
  switch (p.proto) {
    case kProtoTcp:
      p.l4 = L4::Tcp;
      decode_tcp(r, l4_len, p.tcp);
      break;
    case kProtoUdp:
      p.l4 = L4::Udp;
      decode_udp(r, l4_len, p.udp);
      break;
    case kProtoIcmp:
    case kProtoIcmp6:
      p.l4 = L4::Icmp;
      decode_icmp(r, p.proto == kProtoIcmp6, p.icmp);
      break;
    default:
      p.l4 = L4::Other;
  }
}

void decode_ipv4(netlab::Reader& r, Packet& p) {
  size_t start = r.pos();
  uint8_t vihl = r.u8();
  size_t ihl = static_cast<size_t>(vihl & 0x0F) * 4;
  if ((vihl >> 4) != 4 || ihl < 20) throw netlab::TruncatedError("bad ipv4 header");
  r.skip(1);  // DSCP/ECN
  uint16_t total = r.u16();
  r.skip(2);  // identification
  uint16_t frag = r.u16();
  p.ttl = r.u8();
  p.proto = r.u8();
  r.skip(2);  // checksum
  p.src = read_ip(r, 4);
  p.dst = read_ip(r, 4);
  r.seek(start + ihl);
  p.is_ip = true;
  p.dont_fragment = frag & 0x4000;
  p.fragment = (frag & 0x1FFF) != 0;
  // With TSO/GSO the capturing host may see total length 0; fall back to the frame.
  p.ip_len = total >= ihl ? total : static_cast<uint32_t>(p.wire_len - start);
  decode_l4(r, p, p.ip_len - static_cast<uint32_t>(ihl));
}

void decode_ipv6(netlab::Reader& r, Packet& p) {
  uint32_t first = r.u32();
  if ((first >> 28) != 6) throw netlab::TruncatedError("bad ipv6 header");
  uint16_t payload_len = r.u16();
  uint8_t next = r.u8();
  p.ttl = r.u8();
  p.src = read_ip(r, 6);
  p.dst = read_ip(r, 6);
  p.is_ip = true;
  p.ip_len = 40u + payload_len;

  // Walk extension headers to reach the transport header.
  uint32_t l4_len = payload_len;
  for (int i = 0; i < 8; ++i) {
    if (next == 0 || next == 43 || next == 60) {  // hop-by-hop, routing, destination options
      size_t start = r.pos();
      uint8_t nh = r.u8();
      size_t len = (static_cast<size_t>(r.u8()) + 1) * 8;
      r.seek(start);
      r.skip(len, "ipv6 extension header");
      next = nh;
      l4_len = l4_len > len ? l4_len - static_cast<uint32_t>(len) : 0;
    } else if (next == 44) {  // fragment header
      uint8_t nh = r.u8();
      r.skip(1);
      uint16_t off = r.u16();
      r.skip(4);
      next = nh;
      l4_len = l4_len > 8 ? l4_len - 8 : 0;
      p.fragment = (off & 0xFFF8) != 0;
    } else {
      break;
    }
  }
  p.proto = next;
  decode_l4(r, p, l4_len);
}

void decode_arp(netlab::Reader& r, Packet& p) {
  uint16_t htype = r.u16();
  uint16_t ptype = r.u16();
  uint8_t hlen = r.u8();
  uint8_t plen = r.u8();
  p.arp.op = r.u16();
  if (htype != 1 || ptype != kEtherIpv4 || hlen != 6 || plen != 4) return;
  r.skip(6);
  p.arp.sender = read_ip(r, 4);
  r.skip(6);
  p.arp.target = read_ip(r, 4);
  p.is_arp = true;
}

void decode_ethertype(netlab::Reader& r, Packet& p, uint16_t ethertype) {
  while (ethertype == kEtherVlan || ethertype == kEtherQinQ) {
    r.skip(2, "vlan tag");
    ethertype = r.u16();
  }
  if (ethertype == kEtherIpv4) decode_ipv4(r, p);
  else if (ethertype == kEtherIpv6) decode_ipv6(r, p);
  else if (ethertype == kEtherArp) decode_arp(r, p);
}

void decode_by_version(netlab::Reader& r, Packet& p) {
  r.need(1, "ip version");
  uint8_t v = r.cursor()[0] >> 4;
  if (v == 4) decode_ipv4(r, p);
  else if (v == 6) decode_ipv6(r, p);
}

std::string dest_unreach_name(uint8_t code) {
  switch (code) {
    case 0: return "dest-unreach/net";
    case 1: return "dest-unreach/host";
    case 3: return "dest-unreach/port";
    case 4: return "dest-unreach/frag-needed";
    case 9:
    case 10:
    case 13: return "dest-unreach/admin-prohibited";
    default: return "dest-unreach/" + std::to_string(code);
  }
}

std::string port_suffix(const IpAddr& ip, uint16_t port) {
  return ip.family == 6 ? "[" + ip.str() + "]." + std::to_string(port) : ip.str() + "." + std::to_string(port);
}

}  // namespace

std::string link_name(int linktype) {
  switch (linktype) {
    case link::Null: return "NULL";
    case link::Ethernet: return "EN10MB";
    case link::RawBsd:
    case link::RawOpenBsd:
    case link::Raw: return "RAW";
    case link::Loop: return "LOOP";
    case link::LinuxSll: return "LINUX_SLL";
    case link::LinuxSll2: return "LINUX_SLL2";
    case link::Ipv4: return "IPV4";
    case link::Ipv6: return "IPV6";
    default: return "LINKTYPE" + std::to_string(linktype);
  }
}

std::string IpAddr::str() const {
  char buf[INET6_ADDRSTRLEN] = {};
  if (family == 4) ::inet_ntop(AF_INET, bytes.data(), buf, sizeof buf);
  else if (family == 6) ::inet_ntop(AF_INET6, bytes.data(), buf, sizeof buf);
  else return "?";
  return buf;
}

bool Icmp::is_error() const {
  if (v6) return type < 128;  // ICMPv6: types 0-127 are errors
  return type == 3 || type == 4 || type == 5 || type == 11 || type == 12;
}

std::string Icmp::name() const {
  if (v6) {
    switch (type) {
      case 1:
        return code == 1 ? "dest-unreach/admin-prohibited" : code == 3 ? "dest-unreach/address" :
               code == 4 ? "dest-unreach/port" : "dest-unreach/" + std::to_string(code);
      case 2: return "packet-too-big";
      case 3: return "time-exceeded";
      case 128: return "echo-request";
      case 129: return "echo-reply";
      case 133: return "router-solicit";
      case 134: return "router-advert";
      case 135: return "neighbor-solicit";
      case 136: return "neighbor-advert";
      default: return "icmp6-type" + std::to_string(type);
    }
  }
  switch (type) {
    case 0: return "echo-reply";
    case 3: return dest_unreach_name(code);
    case 5: return "redirect";
    case 8: return "echo-request";
    case 11: return code == 0 ? "time-exceeded/ttl" : "time-exceeded/reassembly";
    default: return "icmp-type" + std::to_string(type) + "/" + std::to_string(code);
  }
}

Packet decode(int linktype, double ts, const uint8_t* data, uint32_t caplen, uint32_t wirelen) {
  Packet p;
  p.ts = ts;
  p.cap_len = caplen;
  p.wire_len = wirelen;
  netlab::Reader r(data, caplen);

  switch (linktype) {
    case link::Ethernet: {
      r.skip(12, "ethernet addresses");
      decode_ethertype(r, p, r.u16());
      break;
    }
    case link::LinuxSll: {  // 16 bytes; protocol in the last 2
      r.skip(14, "sll header");
      decode_ethertype(r, p, r.u16());
      break;
    }
    case link::LinuxSll2: {  // 20 bytes; protocol first
      uint16_t proto = r.u16();
      r.skip(18, "sll2 header");
      decode_ethertype(r, p, proto);
      break;
    }
    case link::Null:
    case link::Loop: {
      r.need(4, "loopback header");
      uint32_t family;
      std::memcpy(&family, r.cursor(), 4);
      if (linktype == link::Loop) family = ntohl(family);
      r.skip(4);
      // AF_INET6 differs between BSDs (24, 28, 30); AF_INET is 2 everywhere.
      if (family == 2) decode_ipv4(r, p);
      else if (family == 24 || family == 28 || family == 30) decode_ipv6(r, p);
      break;
    }
    case link::RawBsd:
    case link::RawOpenBsd:
    case link::Raw:
      decode_by_version(r, p);
      break;
    case link::Ipv4:
      decode_ipv4(r, p);
      break;
    case link::Ipv6:
      decode_ipv6(r, p);
      break;
    default:
      throw std::invalid_argument("unsupported link type " + link_name(linktype));
  }
  return p;
}

std::string describe(const Packet& p) {
  char ts[32];
  std::snprintf(ts, sizeof ts, "%.6f", p.ts);
  std::string out = ts;

  if (p.is_arp) {
    if (p.arp.op == 1) return out + " ARP who-has " + p.arp.target.str() + " tell " + p.arp.sender.str();
    return out + " ARP reply " + p.arp.sender.str() + " is-at (op " + std::to_string(p.arp.op) + ")";
  }
  if (!p.is_ip) return out + " non-IP frame, " + std::to_string(p.wire_len) + " bytes";

  std::string ipv = p.src.family == 4 ? " IP " : " IP6 ";
  switch (p.l4) {
    case L4::Tcp: {
      const Tcp& t = p.tcp;
      std::string flags;
      if (t.has(tcpflag::SYN)) flags += 'S';
      if (t.has(tcpflag::FIN)) flags += 'F';
      if (t.has(tcpflag::RST)) flags += 'R';
      if (t.has(tcpflag::PSH)) flags += 'P';
      if (t.has(tcpflag::ACK)) flags += '.';
      out += ipv + port_suffix(p.src, t.sport) + " > " + port_suffix(p.dst, t.dport) + ": Flags [" + flags +
             "], seq " + std::to_string(t.seq);
      if (t.has(tcpflag::ACK)) out += ", ack " + std::to_string(t.ack);
      out += ", win " + std::to_string(t.window);
      if (t.mss) out += ", mss " + std::to_string(t.mss);
      if (t.wscale >= 0) out += ", wscale " + std::to_string(t.wscale);
      out += ", length " + std::to_string(t.payload_len);
      return out;
    }
    case L4::Udp:
      return out + ipv + port_suffix(p.src, p.udp.sport) + " > " + port_suffix(p.dst, p.udp.dport) +
             ": UDP, length " + std::to_string(p.udp.payload_len);
    case L4::Icmp: {
      out += ipv + p.src.str() + " > " + p.dst.str() + ": ICMP " + p.icmp.name();
      if (p.icmp.mtu) out += " mtu " + std::to_string(p.icmp.mtu);
      if (p.icmp.has_inner) out += " (for " + p.icmp.inner_dst.str() + ")";
      return out;
    }
    default:
      return out + ipv + p.src.str() + " > " + p.dst.str() + ": proto " + std::to_string(p.proto) +
             (p.fragment ? " fragment" : "") + ", length " + std::to_string(p.ip_len);
  }
}

}  // namespace pkt
