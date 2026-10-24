// Unit tests for pktcore: frames are built byte-by-byte so each test states
// exactly what's on the wire.

#include <cstdint>
#include <string>
#include <vector>

#include "analyzer.hpp"
#include "netlab/check.hpp"
#include "packet.hpp"
#include "tls.hpp"

namespace {

using Bytes = std::vector<uint8_t>;
using IPv4 = std::array<uint8_t, 4>;

const IPv4 kClient = {10, 0, 0, 1};
const IPv4 kServer = {10, 0, 0, 2};
const IPv4 kRouter = {10, 0, 0, 254};

void put16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}
void put32(Bytes& b, uint32_t v) {
  put16(b, static_cast<uint16_t>(v >> 16));
  put16(b, static_cast<uint16_t>(v));
}

Bytes ipv4(const IPv4& src, const IPv4& dst, uint8_t proto, const Bytes& l4) {
  Bytes b = {0x45, 0};
  put16(b, static_cast<uint16_t>(20 + l4.size()));
  put16(b, 0);
  put16(b, 0x4000);  // DF
  b.push_back(64);
  b.push_back(proto);
  put16(b, 0);
  b.insert(b.end(), src.begin(), src.end());
  b.insert(b.end(), dst.begin(), dst.end());
  b.insert(b.end(), l4.begin(), l4.end());
  return b;
}

Bytes ethernet(const Bytes& ip) {
  Bytes b(12, 0xAA);
  put16(b, 0x0800);
  b.insert(b.end(), ip.begin(), ip.end());
  return b;
}

Bytes tcp(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win,
          const Bytes& payload = {}, bool syn_options = false) {
  Bytes opts;
  if (syn_options) opts = {2, 4, 0x05, 0xB4, 1, 3, 3, 7, 4, 2, 1, 1};  // MSS 1460, NOP, WS 7, SACK-perm, NOP NOP
  Bytes b;
  put16(b, sport);
  put16(b, dport);
  put32(b, seq);
  put32(b, ack);
  b.push_back(static_cast<uint8_t>(((20 + opts.size()) / 4) << 4));
  b.push_back(flags);
  put16(b, win);
  put32(b, 0);
  b.insert(b.end(), opts.begin(), opts.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

Bytes udp(uint16_t sport, uint16_t dport, const Bytes& payload) {
  Bytes b;
  put16(b, sport);
  put16(b, dport);
  put16(b, static_cast<uint16_t>(8 + payload.size()));
  put16(b, 0);
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

struct Feeder {
  pkt::Analyzer a{pkt::link::Ethernet};
  double t = 1000.0;
  bool send(const IPv4& src, const IPv4& dst, uint8_t proto, const Bytes& l4, double dt = 0.010) {
    t += dt;
    Bytes f = ethernet(ipv4(src, dst, proto, l4));
    return a.feed(t, f.data(), static_cast<uint32_t>(f.size()), static_cast<uint32_t>(f.size()));
  }
  bool c2s(const Bytes& seg, double dt = 0.010) { return send(kClient, kServer, 6, seg, dt); }
  bool s2c(const Bytes& seg, double dt = 0.010) { return send(kServer, kClient, 6, seg, dt); }
};

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

using namespace pkt::tcpflag;

void test_decode_syn_options() {
  Bytes f = ethernet(ipv4(kClient, kServer, 6, tcp(51000, 443, 1000, 0, SYN, 64240, {}, true)));
  pkt::Packet p = pkt::decode(pkt::link::Ethernet, 1.0, f.data(), static_cast<uint32_t>(f.size()),
                              static_cast<uint32_t>(f.size()));
  CHECK(p.is_ip);
  CHECK(p.l4 == pkt::L4::Tcp);
  CHECK_EQ(p.src.str(), std::string("10.0.0.1"));
  CHECK_EQ(p.tcp.dport, 443);
  CHECK_EQ(p.tcp.mss, 1460);
  CHECK_EQ(p.tcp.wscale, 7);
  CHECK(p.tcp.sack_permitted);
  CHECK(p.dont_fragment);
  CHECK_EQ(p.tcp.payload_len, 0u);
}

void test_truncated_frame_is_counted() {
  Bytes f = ethernet(ipv4(kClient, kServer, 6, tcp(1, 2, 0, 0, SYN, 0)));
  f.resize(30);  // cut inside the IP header
  pkt::Analyzer a(pkt::link::Ethernet);
  CHECK(!a.feed(1.0, f.data(), static_cast<uint32_t>(f.size()), 60));
  CHECK(contains(a.summary_json(), "\"decode_errors\":1"));
}

void test_full_connection_with_retransmission() {
  Feeder f;
  Bytes data(1000, 'x');
  f.c2s(tcp(51000, 80, 100, 0, SYN, 64240, {}, true));
  f.s2c(tcp(80, 51000, 5000, 101, SYN | ACK, 65160, {}, true), 0.020);
  f.c2s(tcp(51000, 80, 101, 5001, ACK, 502), 0.020);
  f.c2s(tcp(51000, 80, 101, 5001, PSH | ACK, 502, Bytes(100, 'g')));
  f.s2c(tcp(80, 51000, 5001, 201, ACK, 509, data), 0.020);       // first data segment, RTT sample source
  f.c2s(tcp(51000, 80, 201, 6001, ACK, 502), 0.020);             // acks it: one RTT sample
  f.s2c(tcp(80, 51000, 6001, 201, ACK, 509, data));
  f.s2c(tcp(80, 51000, 6001, 201, ACK, 509, data), 0.200);       // same bytes again: retransmission
  f.c2s(tcp(51000, 80, 201, 7001, ACK, 502));
  f.c2s(tcp(51000, 80, 201, 7001, FIN | ACK, 502));
  f.s2c(tcp(80, 51000, 7001, 202, FIN | ACK, 509));
  f.c2s(tcp(51000, 80, 202, 7002, ACK, 502));

  const auto& flows = f.a.tcp_flows();
  CHECK_EQ(flows.size(), 1u);
  const pkt::TcpFlow& flow = flows[0];
  CHECK_EQ(flow.state(), std::string("closed"));
  CHECK_EQ(flow.client.port, 51000);
  CHECK_EQ(flow.dir[1].retransmissions, 1u);
  CHECK_EQ(flow.dir[0].retransmissions, 0u);
  CHECK_EQ(flow.dir[1].bytes_acked, 2001u);  // 2000 data bytes + FIN
  CHECK_EQ(flow.dir[0].mss, 1460);
  CHECK(flow.rtt_ms[1].count >= 1);  // server data acked by the client
  CHECK_EQ(flow.rtt_ms[0].count, 1u);  // the client's request, acked by the server's response
  std::string json = f.a.summary_json();
  CHECK(contains(json, "\"state\":\"closed\""));
  CHECK(contains(json, "\"syn_to_synack\":20"));
  CHECK(contains(json, "\"retransmissions\":[0,1]"));
}

void test_syn_unanswered() {
  Feeder f;
  f.c2s(tcp(52000, 8080, 7, 0, SYN, 64240));
  f.c2s(tcp(52000, 8080, 7, 0, SYN, 64240), 1.0);  // retransmitted SYN, same ISN
  f.c2s(tcp(52000, 8080, 7, 0, SYN, 64240), 2.0);
  const auto& flows = f.a.tcp_flows();
  CHECK_EQ(flows.size(), 1u);
  CHECK_EQ(flows[0].syns, 3u);
  CHECK_EQ(flows[0].state(), std::string("syn_unanswered"));
  CHECK_EQ(flows[0].dir[0].retransmissions, 0u);  // SYN retries are counted in syns, not here
}

void test_refused() {
  Feeder f;
  f.c2s(tcp(53000, 9, 1, 0, SYN, 64240));
  f.s2c(tcp(9, 53000, 0, 2, RST | ACK, 0));
  CHECK_EQ(f.a.tcp_flows()[0].state(), std::string("refused"));
}

void test_port_reuse_starts_new_flow() {
  Feeder f;
  f.c2s(tcp(54000, 80, 1, 0, SYN, 1000));
  f.s2c(tcp(80, 54000, 0, 2, RST | ACK, 0));
  f.c2s(tcp(54000, 80, 99999, 0, SYN, 1000), 1.0);  // new ISN after the reset
  CHECK_EQ(f.a.tcp_flows().size(), 2u);
}

void test_dns_nxdomain_and_unanswered() {
  Feeder f;
  Bytes query = {0xAB, 0xCD, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 4, 'n', 'o', 'p', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
  Bytes reply = query;
  reply[2] = 0x81;
  reply[3] = 0x83;  // QR, RD, RA, rcode 3 = NXDOMAIN
  f.send(kClient, kServer, 17, udp(40000, 53, query));
  f.send(kServer, kClient, 17, udp(53, 40000, reply), 0.015);

  Bytes lost = query;
  lost[1] = 0xCE;  // different id, never answered
  f.send(kClient, kServer, 17, udp(40001, 53, lost));

  std::string json = f.a.summary_json();
  CHECK(contains(json, "\"NXDOMAIN\":1"));
  CHECK(contains(json, "\"queries\":2"));
  CHECK(contains(json, "\"unanswered\":[{\"server\":\"10.0.0.2\",\"name\":\"nope.com.\""));
  CHECK(contains(json, "\"failures\":[{\"name\":\"nope.com.\",\"qtype\":\"A\",\"rcode\":\"NXDOMAIN\""));
}

void test_icmp_frag_needed() {
  // ICMP type 3 code 4, next-hop MTU 1400, quoting the TCP packet it refused.
  Bytes quoted = ipv4(kServer, kClient, 6, tcp(443, 51000, 1, 1, ACK, 100));
  quoted.resize(28);
  Bytes icmp = {3, 4, 0, 0, 0, 0};
  put16(icmp, 1400);
  icmp.insert(icmp.end(), quoted.begin(), quoted.end());
  Feeder f;
  f.send(kRouter, kServer, 1, icmp);
  std::string json = f.a.summary_json();
  CHECK(contains(json, "\"kind\":\"dest-unreach/frag-needed\""));
  CHECK(contains(json, "\"mtu\":1400"));
  CHECK(contains(json, "\"about\":\"tcp 10.0.0.2:443 > 10.0.0.1:51000\""));
}

void test_tls_sni_and_alert() {
  // Minimal ClientHello carrying only an SNI extension for "app.lab".
  Bytes sni_ext = {0x00, 0x00};
  Bytes name = {'a', 'p', 'p', '.', 'l', 'a', 'b'};
  Bytes list = {0x00};
  put16(list, static_cast<uint16_t>(name.size()));
  list.insert(list.end(), name.begin(), name.end());
  Bytes ext_body;
  put16(ext_body, static_cast<uint16_t>(list.size()));
  ext_body.insert(ext_body.end(), list.begin(), list.end());
  put16(sni_ext, static_cast<uint16_t>(ext_body.size()));
  sni_ext.insert(sni_ext.end(), ext_body.begin(), ext_body.end());

  Bytes hello = {0x03, 0x03};
  hello.insert(hello.end(), 32, 0x11);              // random
  hello.push_back(0);                               // session id
  put16(hello, 2), put16(hello, 0x1301);            // one cipher suite
  hello.push_back(1), hello.push_back(0);           // compression: null
  put16(hello, static_cast<uint16_t>(sni_ext.size()));
  hello.insert(hello.end(), sni_ext.begin(), sni_ext.end());

  Bytes handshake = {1, 0, 0, static_cast<uint8_t>(hello.size())};
  handshake.insert(handshake.end(), hello.begin(), hello.end());
  Bytes record = {22, 3, 1};
  put16(record, static_cast<uint16_t>(handshake.size()));
  record.insert(record.end(), handshake.begin(), handshake.end());

  tls::Info info;
  tls::inspect(record.data(), record.size(), 0, info);
  CHECK(info.client_hello);
  CHECK_EQ(info.sni, std::string("app.lab"));

  Bytes alert = {21, 3, 3, 0, 2, 2, 48};  // fatal unknown_ca
  tls::inspect(alert.data(), alert.size(), 1, info);
  CHECK_EQ(info.alerts.size(), 1u);
  CHECK_EQ(info.alerts[0].name(), std::string("unknown_ca"));

  Bytes not_tls = {'G', 'E', 'T', ' ', '/'};
  tls::Info other;
  tls::inspect(not_tls.data(), not_tls.size(), 0, other);
  CHECK(!other.seen());
}

}  // namespace

int main() {
  test_decode_syn_options();
  test_truncated_frame_is_counted();
  test_full_connection_with_retransmission();
  test_syn_unanswered();
  test_refused();
  test_port_reuse_starts_new_flow();
  test_dns_nxdomain_and_unanswered();
  test_icmp_frag_needed();
  test_tls_sni_and_alert();
  return netlab::test::finish();
}
