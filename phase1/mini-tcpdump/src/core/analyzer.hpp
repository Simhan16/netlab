#pragma once

// Stateful analysis over a packet stream: TCP/UDP flow tables, TCP health
// signals, DNS transactions, ICMP errors and ARP. Produces a JSON summary that
// is small enough to hand to an LLM (PacketWhisperer) and a text report.
//
// Everything here is measured *at the capture point*. An RTT computed from a
// capture next to the client is the full round trip; from a router in the
// middle it is only that router's share. Diagnoses must keep that in mind.

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "packet.hpp"
#include "tls.hpp"

namespace pkt {

struct Endpoint {
  IpAddr ip;
  uint16_t port = 0;

  std::string str() const;
  bool operator==(const Endpoint& o) const { return port == o.port && ip == o.ip; }
  bool operator<(const Endpoint& o) const { return ip == o.ip ? port < o.port : ip < o.ip; }
};

struct RunningStats {
  uint64_t count = 0;
  double min = 0, max = 0, sum = 0;
  void add(double v);
  double avg() const { return count ? sum / static_cast<double>(count) : 0; }
};

// One direction of a TCP connection. Direction 0 = client -> server.
struct TcpDirection {
  uint64_t packets = 0;
  uint64_t payload_bytes = 0;
  uint32_t max_payload = 0;
  uint32_t retransmissions = 0;
  uint64_t retransmitted_bytes = 0;
  uint32_t dup_acks = 0;     // sent *by* this side: "I'm still missing something"
  uint32_t zero_windows = 0; // advertised by this side: "my receive buffer is full"
  uint32_t rsts = 0;
  uint32_t fins = 0;
  uint16_t mss = 0;
  int wscale = -1;

  // Sequence tracking for data sent by this side.
  bool seq_init = false;
  bool isn_from_syn = false;  // the SYN's sequence number isn't data
  uint32_t isn = 0;
  uint32_t next_seq = 0;      // highest seq + len seen so far
  uint64_t bytes_acked = 0;   // progress acknowledged by the other side
  std::deque<std::pair<uint32_t, double>> unacked;  // (seq end, send time) for RTT sampling

  // ACK tracking for acks sent by this side.
  bool ack_init = false;
  uint32_t last_ack = 0;
  uint16_t last_window = 0;
};

struct TcpFlow {
  Endpoint client, server;
  double first_ts = 0, last_ts = 0;
  TcpDirection dir[2];
  uint32_t syns = 0, synacks = 0;
  double syn_ts = -1, synack_ts = -1, established_ts = -1;
  // RTT samples keyed by whose data was acknowledged. From a capture next to
  // the client, rtt_ms[0] (client data -> server ACK) is the network round
  // trip, while rtt_ms[1] is only the client's own ACK delay. Mixing the two
  // would hide exactly the latency we're looking for.
  RunningStats rtt_ms[2];
  tls::Info tls;

  bool saw_syn() const { return syns > 0; }
  bool established() const { return established_ts >= 0; }
  std::string state() const;
};

struct UdpFlow {
  Endpoint client, server;
  double first_ts = 0, last_ts = 0;
  uint64_t packets[2] = {0, 0};
  uint64_t bytes[2] = {0, 0};
};

class Analyzer {
 public:
  explicit Analyzer(int linktype) : linktype_(linktype) {}

  // Decodes and accounts one frame. Returns false if it couldn't be decoded
  // (counted, never thrown: real captures always contain some junk).
  bool feed(double ts, const uint8_t* data, uint32_t caplen, uint32_t wirelen);
  const Packet& last_packet() const { return last_; }

  std::string summary_json() const;
  std::string summary_text(size_t max_flows = 20) const;

  uint64_t packets() const { return packets_; }
  const std::vector<TcpFlow>& tcp_flows() const { return tcp_; }

 private:
  struct DnsKey {
    Endpoint client, server;
    uint16_t id;
    bool operator<(const DnsKey& o) const;
  };
  struct DnsPending {
    double ts;
    std::string name;
    uint16_t qtype;
  };
  struct DnsFailure {
    std::string name, server;
    uint16_t qtype;
    uint8_t rcode;
    uint32_t count;
  };
  struct IcmpEvent {
    double ts;
    std::string from, to, kind, about;
    uint32_t mtu;
  };
  struct ArpTarget {
    uint32_t requests = 0;
    bool answered = false;
  };

  void on_tcp(const Packet& p);
  void on_udp(const Packet& p);
  void on_dns(const Packet& p, const Endpoint& src, const Endpoint& dst);
  void on_icmp(const Packet& p);
  void on_arp(const Packet& p);

  int linktype_;
  Packet last_;

  uint64_t packets_ = 0, bytes_ = 0, decode_errors_ = 0, truncated_ = 0, non_ip_ = 0;
  double first_ts_ = -1, last_ts_ = 0;
  std::map<std::string, uint64_t> proto_counts_;

  std::vector<TcpFlow> tcp_;
  std::map<std::pair<Endpoint, Endpoint>, size_t> tcp_index_;  // canonical (low, high) -> flow
  std::vector<UdpFlow> udp_;
  std::map<std::pair<Endpoint, Endpoint>, size_t> udp_index_;

  uint64_t dns_queries_ = 0, dns_responses_ = 0, dns_unmatched_ = 0, dns_malformed_ = 0;
  std::map<std::string, uint64_t> dns_rcodes_;
  RunningStats dns_latency_ms_;
  std::map<DnsKey, DnsPending> dns_pending_;
  std::map<std::string, uint64_t> dns_names_;
  std::vector<DnsFailure> dns_failures_;

  std::map<std::string, uint64_t> icmp_counts_;
  std::vector<IcmpEvent> icmp_events_;
  uint64_t echo_requests_ = 0, echo_replies_ = 0;

  uint64_t arp_requests_ = 0, arp_replies_ = 0;
  std::map<IpAddr, ArpTarget> arp_targets_;
};

}  // namespace pkt
