#include "analyzer.hpp"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <tuple>

#include "dns.hpp"
#include "json.hpp"
#include "netlab/bytes.hpp"

namespace pkt {

namespace {

constexpr size_t kMaxUnacked = 1024;
constexpr size_t kMaxIcmpEvents = 50;
constexpr size_t kMaxDnsFailures = 50;
constexpr size_t kMaxDnsPending = 100000;
constexpr size_t kMaxJsonTcpFlows = 2000;
constexpr size_t kMaxJsonUdpFlows = 100;
constexpr uint64_t kTlsInspectSegments = 8;  // handshake fits easily; later segments are ciphertext

// TCP sequence numbers wrap at 2^32, so "a before b" is a signed difference.
bool seq_before(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

template <typename T>
std::pair<T, T> canonical_pair(const T& a, const T& b) {
  return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

template <typename A, typename B>
void pair_field(Json& j, const char* key, const A& a, const B& b) {
  j.key(key).begin_array().value(a).value(b).end_array();
}

void stats_field(Json& j, const char* key, const RunningStats& s) {
  j.key(key).begin_object();
  j.field("samples", s.count).field("min", s.min).field("avg", s.avg()).field("max", s.max);
  j.end_object();
}

}  // namespace

std::string Endpoint::str() const {
  return ip.family == 6 ? "[" + ip.str() + "]:" + std::to_string(port) : ip.str() + ":" + std::to_string(port);
}

void RunningStats::add(double v) {
  if (count == 0 || v < min) min = v;
  if (count == 0 || v > max) max = v;
  sum += v;
  ++count;
}

std::string TcpFlow::state() const {
  const TcpDirection& c = dir[0];
  const TcpDirection& s = dir[1];
  bool reset = c.rsts || s.rsts;
  if (saw_syn()) {
    if (synacks == 0) return s.rsts ? "refused" : "syn_unanswered";
    if (!established()) return reset ? "reset" : "synack_unacked";
    if (reset) return "reset";
    if (c.fins && s.fins) return "closed";
    if (c.fins || s.fins) return "half_closed";
    return "open";
  }
  if (reset) return "midstream_reset";
  if (c.fins && s.fins) return "midstream_closed";
  return "midstream";
}

bool Analyzer::DnsKey::operator<(const DnsKey& o) const {
  return std::tie(client, server, id) < std::tie(o.client, o.server, o.id);
}

bool Analyzer::feed(double ts, const uint8_t* data, uint32_t caplen, uint32_t wirelen) {
  ++packets_;
  bytes_ += wirelen;
  if (caplen < wirelen) ++truncated_;
  if (first_ts_ < 0) first_ts_ = ts;
  last_ts_ = std::max(last_ts_, ts);

  try {
    last_ = decode(linktype_, ts, data, caplen, wirelen);
  } catch (const netlab::TruncatedError&) {
    ++decode_errors_;
    return false;
  }

  if (last_.is_arp) {
    ++proto_counts_["arp"];
    on_arp(last_);
  } else if (!last_.is_ip) {
    ++non_ip_;
    ++proto_counts_["other"];
  } else {
    switch (last_.l4) {
      case L4::Tcp:
        ++proto_counts_["tcp"];
        on_tcp(last_);
        break;
      case L4::Udp:
        ++proto_counts_["udp"];
        on_udp(last_);
        break;
      case L4::Icmp:
        ++proto_counts_[last_.icmp.v6 ? "icmp6" : "icmp"];
        on_icmp(last_);
        break;
      default:
        ++proto_counts_[last_.fragment ? "ip_fragment" : "ip_other"];
    }
  }
  return true;
}

void Analyzer::on_tcp(const Packet& p) {
  const Tcp& t = p.tcp;
  const bool syn = t.has(tcpflag::SYN), ack = t.has(tcpflag::ACK);
  const bool fin = t.has(tcpflag::FIN), rst = t.has(tcpflag::RST);
  Endpoint src{p.src, t.sport}, dst{p.dst, t.dport};
  auto key = canonical_pair(src, dst);

  auto it = tcp_index_.find(key);
  bool new_flow = it == tcp_index_.end();
  if (!new_flow && syn && !ack) {
    // Same 4-tuple, new ISN: the port was reused for a fresh connection.
    const TcpFlow& old = tcp_[it->second];
    bool finished = old.dir[0].rsts || old.dir[1].rsts || (old.dir[0].fins && old.dir[1].fins);
    bool different_isn = old.dir[0].seq_init && t.seq != old.dir[0].isn;
    if (different_isn && (finished || !old.established())) new_flow = true;
  }
  if (new_flow) {
    TcpFlow f;
    // A SYN tells us who the client is. Mid-stream, guess that the lower
    // port is the service (443 vs 51234).
    bool src_is_client = syn ? !ack : t.dport <= t.sport;
    f.client = src_is_client ? src : dst;
    f.server = src_is_client ? dst : src;
    f.first_ts = p.ts;
    tcp_index_[key] = tcp_.size();
    tcp_.push_back(std::move(f));
    it = tcp_index_.find(key);
  }

  TcpFlow& f = tcp_[it->second];
  f.last_ts = p.ts;
  const int d = src == f.client ? 0 : 1;
  TcpDirection& me = f.dir[d];
  TcpDirection& peer = f.dir[1 - d];

  ++me.packets;
  me.payload_bytes += t.payload_len;
  me.max_payload = std::max(me.max_payload, t.payload_len);
  if (rst) ++me.rsts;
  if (fin) ++me.fins;
  if (!rst && !syn && t.window == 0) ++me.zero_windows;

  if (syn) {
    me.mss = t.mss;
    me.wscale = t.wscale;
    if (!ack) {
      ++f.syns;
      if (f.syn_ts < 0) f.syn_ts = p.ts;
    } else {
      ++f.synacks;
      if (f.synack_ts < 0) f.synack_ts = p.ts;
    }
    if (!me.seq_init) {
      me.seq_init = true;
      me.isn = t.seq;
      me.isn_from_syn = true;
      me.next_seq = t.seq + 1;  // the SYN consumes one sequence number
    }
  } else if (!me.seq_init && !rst) {
    me.seq_init = true;  // joined mid-stream
    me.isn = t.seq;
    me.next_seq = t.seq;
  }

  // Data (and FIN, which also consumes a sequence number). A segment that
  // starts before the highest byte already seen is sending old data again.
  // From one capture point, loss-and-resend and reordering look the same;
  // reordering is rare enough that we call it a retransmission.
  uint32_t seg_len = t.payload_len + (fin ? 1 : 0);
  if (!syn && seg_len > 0 && me.seq_init) {
    uint32_t end = t.seq + seg_len;
    if (seq_before(t.seq, me.next_seq)) {
      ++me.retransmissions;
      me.retransmitted_bytes += t.payload_len;
      me.unacked.clear();  // Karn's rule: an ACK after a resend is an ambiguous RTT sample
      if (seq_before(me.next_seq, end)) me.next_seq = end;
    } else {
      me.next_seq = end;
      if (t.payload_len > 0 && me.unacked.size() < kMaxUnacked) me.unacked.emplace_back(end, p.ts);
    }
  }

  if (ack) {
    // RTT sample: when we see the ACK covering a segment we saw earlier.
    double sent_ts = -1;
    while (!peer.unacked.empty() && !seq_before(t.ack, peer.unacked.front().first)) {
      sent_ts = peer.unacked.front().second;
      peer.unacked.pop_front();
    }
    if (sent_ts >= 0) f.rtt_ms[1 - d].add((p.ts - sent_ts) * 1000.0);

    if (peer.seq_init && !seq_before(t.ack, peer.isn) && !seq_before(peer.next_seq, t.ack)) {
      uint32_t progress = t.ack - peer.isn;
      if (peer.isn_from_syn && progress > 0) --progress;
      peer.bytes_acked = std::max<uint64_t>(peer.bytes_acked, progress);
    }

    // Duplicate ACK: same ack and window again while data is outstanding.
    // Three in a row trigger the sender's fast retransmit.
    bool pure_ack = t.payload_len == 0 && !syn && !fin && !rst;
    if (pure_ack && me.ack_init && t.ack == me.last_ack && t.window == me.last_window && peer.seq_init &&
        seq_before(t.ack, peer.next_seq)) {
      ++me.dup_acks;
    }
    me.ack_init = true;
    me.last_ack = t.ack;
    me.last_window = t.window;

    if (d == 0 && !syn && f.synack_ts >= 0 && f.established_ts < 0) f.established_ts = p.ts;
  }

  if (t.payload_captured > 0 && me.packets <= kTlsInspectSegments + 2) {
    tls::inspect(t.payload, t.payload_captured, d, f.tls);
  }
}

void Analyzer::on_udp(const Packet& p) {
  const Udp& u = p.udp;
  Endpoint src{p.src, u.sport}, dst{p.dst, u.dport};
  auto key = canonical_pair(src, dst);
  auto it = udp_index_.find(key);
  if (it == udp_index_.end()) {
    UdpFlow f;
    bool src_is_client = !(u.sport == 53 && u.dport != 53);  // a reply seen first
    f.client = src_is_client ? src : dst;
    f.server = src_is_client ? dst : src;
    f.first_ts = p.ts;
    it = udp_index_.emplace(key, udp_.size()).first;
    udp_.push_back(f);
  }
  UdpFlow& f = udp_[it->second];
  f.last_ts = p.ts;
  int d = src == f.client ? 0 : 1;
  ++f.packets[d];
  f.bytes[d] += u.payload_len;

  if (u.sport == 53 || u.dport == 53 || u.sport == 5353 || u.dport == 5353) on_dns(p, src, dst);
}

void Analyzer::on_dns(const Packet& p, const Endpoint& src, const Endpoint& dst) {
  dns::Message m;
  try {
    m = dns::parse(p.udp.payload, p.udp.payload_captured);
  } catch (const dns::ParseError&) {
    ++dns_malformed_;
    return;
  }
  std::string name = m.questions.empty() ? "" : m.questions[0].name;
  uint16_t qtype = m.questions.empty() ? 0 : m.questions[0].qtype;

  if (!m.header.is_response()) {
    ++dns_queries_;
    ++dns_names_[name];
    if (dns_pending_.size() < kMaxDnsPending) dns_pending_.emplace(DnsKey{src, dst, m.header.id}, DnsPending{p.ts, name, qtype});
    return;
  }

  ++dns_responses_;
  uint8_t rc = m.header.rcode();
  ++dns_rcodes_[dns::rcode_to_string(rc)];
  auto it = dns_pending_.find(DnsKey{dst, src, m.header.id});
  if (it == dns_pending_.end()) {
    ++dns_unmatched_;
  } else {
    dns_latency_ms_.add((p.ts - it->second.ts) * 1000.0);
    dns_pending_.erase(it);
  }
  if (rc != dns::rcode::NOERROR) {
    std::string server = src.ip.str();
    for (auto& f : dns_failures_) {
      if (f.name == name && f.qtype == qtype && f.rcode == rc && f.server == server) {
        ++f.count;
        return;
      }
    }
    if (dns_failures_.size() < kMaxDnsFailures) dns_failures_.push_back({name, server, qtype, rc, 1});
  }
}

void Analyzer::on_icmp(const Packet& p) {
  const Icmp& ic = p.icmp;
  ++icmp_counts_[ic.name()];
  if (ic.is_echo_request()) ++echo_requests_;
  if (ic.is_echo_reply()) ++echo_replies_;
  if (!ic.is_error() || icmp_events_.size() >= kMaxIcmpEvents) return;

  std::string about;
  if (ic.has_inner) {
    std::string proto = ic.inner_proto == 6 ? "tcp" : ic.inner_proto == 17 ? "udp" : "proto" + std::to_string(ic.inner_proto);
    about = proto + " " + Endpoint{ic.inner_src, ic.inner_sport}.str() + " > " + Endpoint{ic.inner_dst, ic.inner_dport}.str();
  }
  icmp_events_.push_back({p.ts, p.src.str(), p.dst.str(), ic.name(), about, ic.mtu});
}

void Analyzer::on_arp(const Packet& p) {
  if (p.arp.op == 1) {
    ++arp_requests_;
    ++arp_targets_[p.arp.target].requests;
  } else if (p.arp.op == 2) {
    ++arp_replies_;
    arp_targets_[p.arp.sender].answered = true;
  }
}

std::string Analyzer::summary_json() const {
  const double t0 = first_ts_ < 0 ? 0 : first_ts_;
  Json j;
  j.begin_object();

  j.key("capture").begin_object();
  j.field("packets", packets_).field("bytes", bytes_).field("duration_s", first_ts_ < 0 ? 0.0 : last_ts_ - t0);
  j.field("linktype", link_name(linktype_)).field("decode_errors", decode_errors_);
  j.field("truncated_frames", truncated_).field("non_ip_frames", non_ip_);
  j.end_object();

  j.key("protocols").begin_object();
  for (const auto& [k, v] : proto_counts_) j.field(k, v);
  j.end_object();

  // --- TCP
  std::vector<size_t> order(tcp_.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return tcp_[a].dir[0].packets + tcp_[a].dir[1].packets > tcp_[b].dir[0].packets + tcp_[b].dir[1].packets;
  });
  std::map<std::string, uint64_t> states;
  uint64_t retrans = 0, data_segments = 0;
  for (const auto& f : tcp_) {
    ++states[f.state()];
    retrans += f.dir[0].retransmissions + f.dir[1].retransmissions;
    data_segments += f.dir[0].packets + f.dir[1].packets;
  }
  j.key("tcp").begin_object();
  j.field("flow_count", tcp_.size()).field("retransmissions", retrans).field("packets", data_segments);
  j.key("states").begin_object();
  for (const auto& [k, v] : states) j.field(k, v);
  j.end_object();
  j.field("flows_omitted", tcp_.size() > kMaxJsonTcpFlows ? tcp_.size() - kMaxJsonTcpFlows : 0);
  j.key("flows").begin_array();
  for (size_t n = 0; n < order.size() && n < kMaxJsonTcpFlows; ++n) {
    const TcpFlow& f = tcp_[order[n]];
    const TcpDirection& c = f.dir[0];
    const TcpDirection& s = f.dir[1];
    j.begin_object();
    j.field("client", f.client.str()).field("server", f.server.str()).field("state", f.state());
    j.field("start_s", f.first_ts - t0).field("duration_s", f.last_ts - f.first_ts);
    pair_field(j, "packets", c.packets, s.packets);
    pair_field(j, "payload_bytes", c.payload_bytes, s.payload_bytes);
    pair_field(j, "bytes_acked", c.bytes_acked, s.bytes_acked);
    pair_field(j, "max_payload", c.max_payload, s.max_payload);
    pair_field(j, "retransmissions", c.retransmissions, s.retransmissions);
    pair_field(j, "retransmitted_bytes", c.retransmitted_bytes, s.retransmitted_bytes);
    pair_field(j, "dup_acks", c.dup_acks, s.dup_acks);
    pair_field(j, "zero_windows", c.zero_windows, s.zero_windows);
    pair_field(j, "rsts", c.rsts, s.rsts);
    pair_field(j, "fins", c.fins, s.fins);
    j.field("syns", f.syns).field("synacks", f.synacks);
    if (c.mss || s.mss) pair_field(j, "mss", c.mss, s.mss);
    if (c.wscale >= 0 || s.wscale >= 0) pair_field(j, "wscale", c.wscale, s.wscale);
    if (f.synack_ts >= 0 && f.syn_ts >= 0) {
      j.key("handshake_ms").begin_object();
      j.field("syn_to_synack", (f.synack_ts - f.syn_ts) * 1000.0);
      if (f.established()) j.field("synack_to_ack", (f.established_ts - f.synack_ts) * 1000.0);
      j.end_object();
    }
    if (f.rtt_ms[0].count || f.rtt_ms[1].count) {
      j.key("rtt_ms").begin_object();
      if (f.rtt_ms[0].count) stats_field(j, "client_data", f.rtt_ms[0]);
      if (f.rtt_ms[1].count) stats_field(j, "server_data", f.rtt_ms[1]);
      j.end_object();
    }
    if (f.tls.seen()) {
      j.key("tls").begin_object();
      j.field("client_hello", f.tls.client_hello).field("server_hello", f.tls.server_hello);
      if (!f.tls.sni.empty()) j.field("sni", f.tls.sni);
      if (!f.tls.version.empty()) j.field("version", f.tls.version);
      j.field("app_data_records", f.tls.app_data_records);
      j.key("alerts").begin_array();
      for (const auto& a : f.tls.alerts) {
        j.begin_object().field("from", a.direction == 0 ? "client" : "server");
        j.field("level", a.level == 2 ? "fatal" : a.level == 1 ? "warning" : "unknown").field("description", a.name());
        j.end_object();
      }
      j.end_array().end_object();
    }
    j.end_object();
  }
  j.end_array().end_object();

  // --- UDP
  std::vector<size_t> uorder(udp_.size());
  std::iota(uorder.begin(), uorder.end(), 0);
  std::stable_sort(uorder.begin(), uorder.end(), [&](size_t a, size_t b) {
    return udp_[a].packets[0] + udp_[a].packets[1] > udp_[b].packets[0] + udp_[b].packets[1];
  });
  j.key("udp").begin_object().field("flow_count", udp_.size()).key("flows").begin_array();
  for (size_t n = 0; n < uorder.size() && n < kMaxJsonUdpFlows; ++n) {
    const UdpFlow& f = udp_[uorder[n]];
    j.begin_object().field("client", f.client.str()).field("server", f.server.str());
    j.field("start_s", f.first_ts - t0).field("duration_s", f.last_ts - f.first_ts);
    pair_field(j, "packets", f.packets[0], f.packets[1]);
    pair_field(j, "payload_bytes", f.bytes[0], f.bytes[1]);
    j.end_object();
  }
  j.end_array().end_object();

  // --- DNS
  struct Unanswered {
    std::string server, name;
    uint16_t qtype;
    uint32_t attempts;
  };
  std::vector<Unanswered> unanswered;
  for (const auto& entry : dns_pending_) {
    const DnsPending& q = entry.second;
    std::string server = entry.first.server.ip.str();
    auto it = std::find_if(unanswered.begin(), unanswered.end(), [&](const Unanswered& u) {
      return u.server == server && u.name == q.name && u.qtype == q.qtype;
    });
    if (it != unanswered.end()) ++it->attempts;
    else unanswered.push_back({server, q.name, q.qtype, 1});
  }
  std::vector<std::pair<std::string, uint64_t>> names(dns_names_.begin(), dns_names_.end());
  std::stable_sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

  j.key("dns").begin_object();
  j.field("queries", dns_queries_).field("responses", dns_responses_);
  j.field("unmatched_responses", dns_unmatched_).field("malformed", dns_malformed_);
  j.key("rcodes").begin_object();
  for (const auto& [k, v] : dns_rcodes_) j.field(k, v);
  j.end_object();
  if (dns_latency_ms_.count) stats_field(j, "latency_ms", dns_latency_ms_);
  j.key("unanswered").begin_array();
  for (size_t i = 0; i < unanswered.size() && i < 50; ++i) {
    const auto& u = unanswered[i];
    j.begin_object().field("server", u.server).field("name", u.name).field("qtype", dns::type_to_string(u.qtype));
    j.field("attempts", u.attempts).end_object();
  }
  j.end_array();
  j.key("failures").begin_array();
  for (const auto& f : dns_failures_) {
    j.begin_object().field("name", f.name).field("qtype", dns::type_to_string(f.qtype));
    j.field("rcode", dns::rcode_to_string(f.rcode)).field("server", f.server).field("count", f.count).end_object();
  }
  j.end_array();
  j.key("top_names").begin_array();
  for (size_t i = 0; i < names.size() && i < 10; ++i) {
    j.begin_object().field("name", names[i].first).field("queries", names[i].second).end_object();
  }
  j.end_array().end_object();

  // --- ICMP
  j.key("icmp").begin_object();
  j.field("echo_requests", echo_requests_).field("echo_replies", echo_replies_);
  j.key("types").begin_object();
  for (const auto& [k, v] : icmp_counts_) j.field(k, v);
  j.end_object();
  j.key("errors").begin_array();
  for (const auto& e : icmp_events_) {
    j.begin_object().field("t_s", e.ts - t0).field("from", e.from).field("to", e.to).field("kind", e.kind);
    if (!e.about.empty()) j.field("about", e.about);
    if (e.mtu) j.field("mtu", e.mtu);
    j.end_object();
  }
  j.end_array().end_object();

  // --- ARP
  j.key("arp").begin_object().field("requests", arp_requests_).field("replies", arp_replies_);
  j.key("unanswered_targets").begin_array();
  for (const auto& [ip, t] : arp_targets_) {
    if (!t.answered && t.requests > 0) j.begin_object().field("ip", ip.str()).field("requests", t.requests).end_object();
  }
  j.end_array().end_object();

  j.end_object();
  return j.str();
}

std::string Analyzer::summary_text(size_t max_flows) const {
  std::string out;
  char line[512];
  auto add = [&](const char* fmt, auto... args) {
    std::snprintf(line, sizeof line, fmt, args...);
    out += line;
  };

  double duration = first_ts_ < 0 ? 0 : last_ts_ - first_ts_;
  add("capture: %llu packets, %llu bytes, %.3f s, linktype %s", static_cast<unsigned long long>(packets_),
      static_cast<unsigned long long>(bytes_), duration, link_name(linktype_).c_str());
  if (decode_errors_) add(", %llu undecodable", static_cast<unsigned long long>(decode_errors_));
  out += "\nprotocols:";
  for (const auto& [k, v] : proto_counts_) add(" %s=%llu", k.c_str(), static_cast<unsigned long long>(v));
  out += "\n";

  if (!tcp_.empty()) {
    std::vector<size_t> order(tcp_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return tcp_[a].dir[0].packets + tcp_[a].dir[1].packets > tcp_[b].dir[0].packets + tcp_[b].dir[1].packets;
    });
    add("\nTCP flows (%zu)  [counts are client/server]\n", tcp_.size());
    add("  %-42s %-15s %11s %15s %9s %9s  %s\n", "client -> server", "state", "packets", "payload", "retrans",
        "rtt ms", "tls");
    for (size_t n = 0; n < order.size() && n < max_flows; ++n) {
      const TcpFlow& f = tcp_[order[n]];
      std::string pair = f.client.str() + " -> " + f.server.str();
      auto ms = [](const RunningStats& s) { return s.count ? std::to_string(static_cast<int>(s.avg() + 0.5)) : "-"; };
      std::string rtt = ms(f.rtt_ms[0]) + "/" + ms(f.rtt_ms[1]);
      std::string tls_note = f.tls.sni;
      for (const auto& a : f.tls.alerts) tls_note += " alert:" + a.name();
      add("  %-42s %-15s %5llu/%-5llu %7llu/%-7llu %4u/%-4u %9s  %s\n", pair.c_str(), f.state().c_str(),
          static_cast<unsigned long long>(f.dir[0].packets), static_cast<unsigned long long>(f.dir[1].packets),
          static_cast<unsigned long long>(f.dir[0].payload_bytes),
          static_cast<unsigned long long>(f.dir[1].payload_bytes), f.dir[0].retransmissions,
          f.dir[1].retransmissions, rtt.c_str(), tls_note.c_str());
    }
    if (tcp_.size() > max_flows) add("  ... %zu more\n", tcp_.size() - max_flows);
  }

  if (dns_queries_ || dns_responses_) {
    add("\nDNS: %llu queries, %llu responses", static_cast<unsigned long long>(dns_queries_),
        static_cast<unsigned long long>(dns_responses_));
    for (const auto& [k, v] : dns_rcodes_) add(", %s=%llu", k.c_str(), static_cast<unsigned long long>(v));
    if (dns_latency_ms_.count) add(", latency avg %.1f ms (max %.1f)", dns_latency_ms_.avg(), dns_latency_ms_.max);
    if (!dns_pending_.empty()) add(", %zu unanswered", dns_pending_.size());
    out += "\n";
    for (const auto& f : dns_failures_) {
      add("  %s %s -> %s from %s (x%u)\n", f.name.c_str(), dns::type_to_string(f.qtype).c_str(),
          dns::rcode_to_string(f.rcode).c_str(), f.server.c_str(), f.count);
    }
  }

  if (!icmp_counts_.empty()) {
    out += "\nICMP:";
    for (const auto& [k, v] : icmp_counts_) add(" %s=%llu", k.c_str(), static_cast<unsigned long long>(v));
    out += "\n";
    for (size_t i = 0; i < icmp_events_.size() && i < 10; ++i) {
      const auto& e = icmp_events_[i];
      add("  %s -> %s %s%s%s\n", e.from.c_str(), e.to.c_str(), e.kind.c_str(), e.about.empty() ? "" : " about ",
          e.about.c_str());
    }
  }

  if (arp_requests_) {
    size_t unanswered = 0;
    for (const auto& [ip, t] : arp_targets_) unanswered += !t.answered && t.requests;
    add("\nARP: %llu requests, %llu replies, %zu targets never answered\n",
        static_cast<unsigned long long>(arp_requests_), static_cast<unsigned long long>(arp_replies_), unanswered);
  }
  return out;
}

}  // namespace pkt
