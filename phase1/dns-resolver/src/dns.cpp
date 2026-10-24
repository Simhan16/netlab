#include "dns.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cctype>
#include <cstdio>

#include "netlab/bytes.hpp"

namespace dns {

namespace {

constexpr size_t kMaxNameLength = 255;
constexpr size_t kMaxLabelLength = 63;
constexpr int kMaxPointerJumps = 64;

struct TypeName {
  uint16_t code;
  const char* name;
};
constexpr TypeName kTypes[] = {{type::A, "A"},     {type::NS, "NS"},     {type::CNAME, "CNAME"},
                               {type::SOA, "SOA"}, {type::PTR, "PTR"},   {type::MX, "MX"},
                               {type::TXT, "TXT"}, {type::AAAA, "AAAA"}, {type::ANY, "ANY"}};

void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

// Names on the wire are length-prefixed labels: "www.example.com" ->
// 3 'www' 7 'example' 3 'com' 0. To save space a name may end in a 2-byte
// pointer (top bits 11) to an earlier offset in the message: "compression".
// A malicious packet can make pointers loop, so we cap the number of jumps.
std::string read_name(netlab::Reader& r) {
  std::string name;
  size_t wire_length = 1;  // the final root label
  bool jumped = false;
  size_t resume_at = 0;
  int jumps = 0;

  for (;;) {
    uint8_t len = r.u8();
    if ((len & 0xC0) == 0xC0) {
      uint16_t offset = static_cast<uint16_t>((len & 0x3F) << 8 | r.u8());
      if (++jumps > kMaxPointerJumps) throw ParseError("compression pointer loop");
      if (!jumped) resume_at = r.pos();
      jumped = true;
      if (offset >= r.size()) throw ParseError("compression pointer out of range");
      r.seek(offset);
      continue;
    }
    if (len & 0xC0) throw ParseError("reserved label type");
    if (len == 0) break;
    wire_length += 1 + len;
    if (wire_length > kMaxNameLength) throw ParseError("name longer than 255 bytes");
    r.need(len, "label");
    name.append(reinterpret_cast<const char*>(r.cursor()), len);
    name += '.';
    r.skip(len);
  }
  if (jumped) r.seek(resume_at);
  return name.empty() ? "." : name;
}

std::string ip_to_string(int family, const uint8_t* bytes) {
  char buf[INET6_ADDRSTRLEN] = {};
  ::inet_ntop(family, bytes, buf, sizeof buf);
  return buf;
}

// Decodes RDATA into presentation format. Names inside RDATA may use
// compression pointers into the whole message, so we parse with the full
// message reader positioned at the RDATA, then jump to its end regardless.
void read_rdata(netlab::Reader& r, Record& rec, uint16_t rdlength) {
  size_t end = r.pos() + rdlength;
  r.need(rdlength, "rdata");
  switch (rec.rtype) {
    case type::A:
      if (rdlength != 4) throw ParseError("A record with rdlength " + std::to_string(rdlength));
      rec.data = ip_to_string(AF_INET, r.cursor());
      break;
    case type::AAAA:
      if (rdlength != 16) throw ParseError("AAAA record with rdlength " + std::to_string(rdlength));
      rec.data = ip_to_string(AF_INET6, r.cursor());
      break;
    case type::NS:
    case type::CNAME:
    case type::PTR:
      rec.target = read_name(r);
      rec.data = rec.target;
      break;
    case type::MX: {
      uint16_t pref = r.u16();
      rec.target = read_name(r);
      rec.data = std::to_string(pref) + " " + rec.target;
      break;
    }
    case type::SOA: {
      std::string mname = read_name(r);
      std::string rname = read_name(r);
      rec.data = mname + " " + rname;
      for (int i = 0; i < 5; ++i) rec.data += " " + std::to_string(r.u32());  // serial refresh retry expire minimum
      break;
    }
    case type::TXT:
      while (r.pos() < end) {
        uint8_t len = r.u8();
        r.need(len, "txt string");
        if (!rec.data.empty()) rec.data += ' ';
        rec.data += '"' + std::string(reinterpret_cast<const char*>(r.cursor()), len) + '"';
        r.skip(len);
      }
      break;
    default: {  // RFC 3597 generic form: \# <len> <hex>
      rec.data = "\\# " + std::to_string(rdlength) + " ";
      char hex[3];
      for (size_t i = 0; i < rdlength; ++i) {
        std::snprintf(hex, sizeof hex, "%02x", r.cursor()[i]);
        rec.data += hex;
      }
    }
  }
  if (r.pos() > end) throw ParseError("rdata overran its length");
  r.seek(end);
}

std::vector<Record> read_records(netlab::Reader& r, uint16_t count) {
  std::vector<Record> out;
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Record rec;
    rec.name = read_name(r);
    rec.rtype = r.u16();
    rec.rclass = r.u16();
    rec.ttl = r.u32();
    uint16_t rdlength = r.u16();
    read_rdata(r, rec, rdlength);
    out.push_back(std::move(rec));
  }
  return out;
}

}  // namespace

std::optional<uint16_t> type_from_string(std::string_view text) {
  std::string upper;
  for (char c : text) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (const auto& t : kTypes) {
    if (upper == t.name) return t.code;
  }
  return std::nullopt;
}

std::string type_to_string(uint16_t t) {
  for (const auto& entry : kTypes) {
    if (entry.code == t) return entry.name;
  }
  return "TYPE" + std::to_string(t);
}

std::string rcode_to_string(uint8_t rc) {
  static const char* names[] = {"NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED"};
  return rc < 6 ? names[rc] : "RCODE" + std::to_string(rc);
}

std::string canonical(std::string_view name) {
  std::string out;
  for (char c : name) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (out.empty() || out.back() != '.') out += '.';
  return out;
}

std::vector<uint8_t> build_query(uint16_t id, std::string_view name, uint16_t qtype, bool recursion_desired) {
  std::vector<uint8_t> out;
  put16(out, id);
  put16(out, recursion_desired ? 0x0100 : 0x0000);  // standard query, RD bit
  put16(out, 1);                                    // QDCOUNT
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);

  if (!name.empty() && name.back() == '.') name.remove_suffix(1);
  size_t total = 1;
  while (!name.empty()) {
    size_t dot = name.find('.');
    std::string_view label = name.substr(0, dot);
    if (label.empty()) throw std::invalid_argument("empty label in name");
    if (label.size() > kMaxLabelLength) throw std::invalid_argument("label longer than 63 bytes");
    total += 1 + label.size();
    if (total > kMaxNameLength) throw std::invalid_argument("name longer than 255 bytes");
    out.push_back(static_cast<uint8_t>(label.size()));
    out.insert(out.end(), label.begin(), label.end());
    name = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
  }
  out.push_back(0);
  put16(out, qtype);
  put16(out, kClassIN);
  return out;
}

Message parse(const uint8_t* data, size_t len) {
  try {
    netlab::Reader r(data, len);
    Message m;
    m.header.id = r.u16();
    m.header.flags = r.u16();
    m.header.qdcount = r.u16();
    m.header.ancount = r.u16();
    m.header.nscount = r.u16();
    m.header.arcount = r.u16();
    for (uint16_t i = 0; i < m.header.qdcount; ++i) {
      Question q;
      q.name = read_name(r);
      q.qtype = r.u16();
      q.qclass = r.u16();
      m.questions.push_back(std::move(q));
    }
    m.answers = read_records(r, m.header.ancount);
    m.authority = read_records(r, m.header.nscount);
    m.additional = read_records(r, m.header.arcount);
    return m;
  } catch (const netlab::TruncatedError& e) {
    throw ParseError(e.what());
  }
}

}  // namespace dns
