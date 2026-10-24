// dnsq: a small dig. Builds DNS packets by hand and talks to servers directly.
//
//   dnsq [@server] name [type] [+trace] [+tcp] [+norec]
//
//   dnsq example.com               ask the system resolver (first nameserver in /etc/resolv.conf)
//   dnsq @1.1.1.1 example.com MX   ask a specific server
//   dnsq example.com +trace        resolve iteratively from a root server, printing each referral

#include <poll.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dns.hpp"
#include "netlab/socket.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kTimeoutMs = 2000;
constexpr int kTries = 3;
constexpr int kMaxTraceSteps = 30;
constexpr int kMaxTraceDepth = 6;

// A handful of root server IPv4 addresses (the full list is the "root hints" file).
const std::vector<std::string> kRootServers = {"198.41.0.4", "192.33.4.12", "193.0.14.129", "202.12.27.33"};

struct Options {
  std::string server;
  std::string name;
  uint16_t qtype = dns::type::A;
  bool trace = false;
  bool tcp = false;
  bool recurse = true;
};

struct Reply {
  dns::Message msg;
  double rtt_ms = 0;
  bool via_tcp = false;
  size_t bytes = 0;
};

std::string system_nameserver() {
  std::ifstream in("/etc/resolv.conf");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key, value;
    if (words >> key >> value && key == "nameserver") return value;
  }
  return "1.1.1.1";
}

uint16_t random_id() {
  // Random IDs (plus random source ports, which the kernel gives us) are the
  // main defense against off-path cache poisoning: an attacker must guess both.
  static std::mt19937 rng(std::random_device{}());
  return static_cast<uint16_t>(rng());
}

bool wait_readable(int fd, int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  int rc;
  do {
    rc = ::poll(&p, 1, timeout_ms);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) netlab::throw_errno("poll");
  return rc > 0;
}

// Only accept a reply that answers *our* question, otherwise a stray or forged
// packet could be taken as the answer.
bool matches(const dns::Message& reply, uint16_t id, const std::string& name, uint16_t qtype) {
  if (!reply.header.is_response() || reply.header.id != id) return false;
  if (reply.questions.size() != 1) return reply.header.rcode() != dns::rcode::NOERROR;  // FORMERR may omit it
  const auto& q = reply.questions[0];
  return dns::canonical(q.name) == dns::canonical(name) && q.qtype == qtype;
}

Reply query_udp(const std::string& server, const std::vector<uint8_t>& packet, uint16_t id,
                const std::string& name, uint16_t qtype) {
  // connect() on UDP filters incoming datagrams to this server's address.
  netlab::Fd fd = netlab::udp_connect(server, 53);
  std::vector<uint8_t> buf(65535);
  for (int attempt = 1; attempt <= kTries; ++attempt) {
    auto t0 = Clock::now();
    if (::send(fd.get(), packet.data(), packet.size(), 0) < 0) netlab::throw_errno("send");
    auto deadline = t0 + std::chrono::milliseconds(kTimeoutMs);
    while (Clock::now() < deadline) {
      int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
      if (!wait_readable(fd.get(), std::max(left, 1))) break;
      ssize_t n = ::recv(fd.get(), buf.data(), buf.size(), 0);
      if (n < 0) {
        if (errno == ECONNREFUSED) throw std::runtime_error(server + ": port unreachable (ICMP)");
        netlab::throw_errno("recv");
      }
      try {
        dns::Message msg = dns::parse(buf.data(), static_cast<size_t>(n));
        if (!matches(msg, id, name, qtype)) continue;  // ignore and keep waiting
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return {std::move(msg), ms, false, static_cast<size_t>(n)};
      } catch (const dns::ParseError& e) {
        std::fprintf(stderr, ";; ignoring malformed reply: %s\n", e.what());
      }
    }
    std::fprintf(stderr, ";; timeout from %s (attempt %d/%d)\n", server.c_str(), attempt, kTries);
  }
  throw std::runtime_error("no response from " + server);
}

bool recv_exact(int fd, uint8_t* out, size_t len) {
  while (len > 0) {
    if (!wait_readable(fd, kTimeoutMs)) return false;
    ssize_t n = ::recv(fd, out, len, 0);
    if (n <= 0) return false;
    out += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// DNS over TCP prefixes each message with a 2-byte length, since TCP is a byte stream.
Reply query_tcp(const std::string& server, const std::vector<uint8_t>& packet, uint16_t id,
                const std::string& name, uint16_t qtype) {
  auto t0 = Clock::now();
  netlab::Fd fd = netlab::tcp_connect(server, 53);
  std::vector<uint8_t> framed = {static_cast<uint8_t>(packet.size() >> 8), static_cast<uint8_t>(packet.size())};
  framed.insert(framed.end(), packet.begin(), packet.end());
  if (!netlab::send_all(fd.get(), framed.data(), framed.size())) netlab::throw_errno("send");

  uint8_t len_buf[2];
  if (!recv_exact(fd.get(), len_buf, 2)) throw std::runtime_error("tcp: no response from " + server);
  std::vector<uint8_t> buf(static_cast<size_t>(len_buf[0] << 8 | len_buf[1]));
  if (!recv_exact(fd.get(), buf.data(), buf.size())) throw std::runtime_error("tcp: short response");
  dns::Message msg = dns::parse(buf);
  if (!matches(msg, id, name, qtype)) throw std::runtime_error("tcp: reply does not match query");
  double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  return {std::move(msg), ms, true, buf.size()};
}

Reply query(const std::string& server, const std::string& name, uint16_t qtype, bool recurse, bool tcp) {
  uint16_t id = random_id();
  std::vector<uint8_t> packet = dns::build_query(id, name, qtype, recurse);
  if (tcp) return query_tcp(server, packet, id, name, qtype);
  Reply reply = query_udp(server, packet, id, name, qtype);
  if (reply.msg.header.truncated()) {
    std::fprintf(stderr, ";; truncated (TC=1), retrying over TCP\n");
    return query_tcp(server, packet, id, name, qtype);
  }
  return reply;
}

void print_section(const char* title, const std::vector<dns::Record>& records) {
  if (records.empty()) return;
  std::printf("\n;; %s SECTION:\n", title);
  for (const auto& r : records) {
    std::printf("%-30s %-7u IN %-6s %s\n", r.name.c_str(), r.ttl, dns::type_to_string(r.rtype).c_str(),
                r.data.c_str());
  }
}

void print_reply(const std::string& server, const Reply& reply) {
  const auto& h = reply.msg.header;
  std::printf(";; ->>HEADER<<- status: %s, id: %u\n", dns::rcode_to_string(h.rcode()).c_str(), h.id);
  std::printf(";; flags:%s%s%s%s%s; QUERY: %u, ANSWER: %u, AUTHORITY: %u, ADDITIONAL: %u\n",
              h.is_response() ? " qr" : "", h.authoritative() ? " aa" : "", h.truncated() ? " tc" : "",
              h.recursion_desired() ? " rd" : "", h.recursion_available() ? " ra" : "", h.qdcount, h.ancount,
              h.nscount, h.arcount);
  print_section("ANSWER", reply.msg.answers);
  print_section("AUTHORITY", reply.msg.authority);
  print_section("ADDITIONAL", reply.msg.additional);
  std::printf("\n;; Query time: %.1f ms\n;; SERVER: %s#53 (%s)\n;; MSG SIZE rcvd: %zu\n", reply.rtt_ms,
              server.c_str(), reply.via_tcp ? "TCP" : "UDP", reply.bytes);
}

// Iterative resolution: ask a root server without RD, follow the NS referral
// it returns to the TLD servers, then to the domain's own servers, until one
// answers authoritatively. This is what a recursive resolver like Unbound does.
std::vector<std::string> trace(const std::string& name, uint16_t qtype, int depth) {
  if (depth > kMaxTraceDepth) throw std::runtime_error("trace: too many nested lookups");
  std::string indent(static_cast<size_t>(depth) * 2, ' ');
  std::vector<std::string> servers = kRootServers;

  for (int step = 0; step < kMaxTraceSteps; ++step) {
    Reply reply;
    std::string asked;
    for (const auto& s : servers) {
      try {
        reply = query(s, name, qtype, /*recurse=*/false, /*tcp=*/false);
        asked = s;
        break;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "%s;; %s\n", indent.c_str(), e.what());
      }
    }
    if (asked.empty()) throw std::runtime_error("trace: no server answered for " + name);

    const auto& m = reply.msg;
    std::printf("%s%s %s @%s -> %s, %zu answer(s), %zu NS (%.1f ms)\n", indent.c_str(), name.c_str(),
                dns::type_to_string(qtype).c_str(), asked.c_str(), dns::rcode_to_string(m.header.rcode()).c_str(),
                m.answers.size(), m.authority.size(), reply.rtt_ms);

    if (m.header.rcode() != dns::rcode::NOERROR) return {};

    std::vector<std::string> results;
    std::string cname;
    for (const auto& r : m.answers) {
      if (dns::canonical(r.name) != dns::canonical(name)) continue;
      if (r.rtype == qtype) results.push_back(r.data);
      if (r.rtype == dns::type::CNAME) cname = r.target;
    }
    if (!results.empty()) return results;
    if (!cname.empty()) {
      std::printf("%s  CNAME -> %s\n", indent.c_str(), cname.c_str());
      return trace(cname, qtype, depth + 1);
    }

    // A referral: NS records in AUTHORITY, ideally with their addresses
    // ("glue") in ADDITIONAL so we don't have to look them up separately.
    std::vector<std::string> ns_names;
    for (const auto& r : m.authority) {
      if (r.rtype == dns::type::NS) ns_names.push_back(dns::canonical(r.target));
    }
    if (ns_names.empty()) return {};  // NOERROR with no data and no referral: name exists, type doesn't

    std::vector<std::string> next;
    for (const auto& r : m.additional) {
      if (r.rtype != dns::type::A) continue;
      for (const auto& ns : ns_names) {
        if (dns::canonical(r.name) == ns) next.push_back(r.data);
      }
    }
    if (next.empty()) {
      std::printf("%s  no glue; resolving %s first\n", indent.c_str(), ns_names[0].c_str());
      next = trace(ns_names[0], dns::type::A, depth + 1);
      if (next.empty()) throw std::runtime_error("trace: could not resolve nameserver " + ns_names[0]);
    }
    servers = next;
  }
  throw std::runtime_error("trace: too many referrals");
}

Options parse_args(int argc, char** argv) {
  Options o;
  bool have_type = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "+trace") {
      o.trace = true;
    } else if (arg == "+tcp") {
      o.tcp = true;
    } else if (arg == "+norec") {
      o.recurse = false;
    } else if (arg[0] == '@') {
      o.server = arg.substr(1);
    } else if (o.name.empty()) {
      o.name = arg;
    } else if (!have_type) {
      auto t = dns::type_from_string(arg);
      if (!t) throw std::invalid_argument("unknown record type: " + arg);
      o.qtype = *t;
      have_type = true;
    } else {
      throw std::invalid_argument("unexpected argument: " + arg);
    }
  }
  if (o.name.empty()) throw std::invalid_argument("usage: dnsq [@server] name [type] [+trace] [+tcp] [+norec]");
  if (o.server.empty()) o.server = system_nameserver();
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options o = parse_args(argc, argv);
    if (o.trace) {
      auto results = trace(o.name, o.qtype, 0);
      std::printf("\n");
      if (results.empty()) std::printf(";; no %s records\n", dns::type_to_string(o.qtype).c_str());
      for (const auto& r : results) std::printf("%s %s %s\n", o.name.c_str(), dns::type_to_string(o.qtype).c_str(), r.c_str());
      return results.empty() ? 1 : 0;
    }
    Reply reply = query(o.server, o.name, o.qtype, o.recurse, o.tcp);
    print_reply(o.server, reply);
    return reply.msg.header.rcode() == dns::rcode::NOERROR ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "dnsq: %s\n", e.what());
    return 2;
  }
}
