#include "tls.hpp"

#include <cstdio>

#include "netlab/bytes.hpp"

namespace tls {

namespace {

constexpr uint8_t kChangeCipherSpec = 20, kAlert = 21, kHandshake = 22, kAppData = 23;
constexpr uint8_t kClientHello = 1, kServerHello = 2;
constexpr uint16_t kExtServerName = 0, kExtSupportedVersions = 43;

void skip_vector(netlab::Reader& r, int len_bytes) {
  size_t n = len_bytes == 1 ? r.u8() : r.u16();
  r.skip(n, "tls vector");
}

void parse_client_hello(netlab::Reader& r, Info& info) {
  info.client_hello = true;
  r.skip(2 + 32, "client version + random");
  skip_vector(r, 1);  // session id
  skip_vector(r, 2);  // cipher suites
  skip_vector(r, 1);  // compression methods
  if (r.remaining() < 2) return;
  size_t ext_end = r.pos() + r.u16();
  while (r.pos() + 4 <= ext_end) {
    uint16_t type = r.u16();
    uint16_t len = r.u16();
    size_t next = r.pos() + len;
    if (type == kExtServerName) {
      r.u16();  // list length
      uint8_t name_type = r.u8();
      uint16_t name_len = r.u16();
      r.need(name_len, "sni");
      if (name_type == 0) info.sni.assign(reinterpret_cast<const char*>(r.cursor()), name_len);
    }
    r.seek(next);
  }
}

void parse_server_hello(netlab::Reader& r, Info& info) {
  info.server_hello = true;
  uint16_t legacy = r.u16();
  info.version = version_name(legacy);
  r.skip(32, "server random");
  skip_vector(r, 1);  // session id
  r.skip(3, "cipher + compression");
  if (r.remaining() < 2) return;
  size_t ext_end = r.pos() + r.u16();
  while (r.pos() + 4 <= ext_end) {
    uint16_t type = r.u16();
    uint16_t len = r.u16();
    size_t next = r.pos() + len;
    // TLS 1.3 keeps 0x0303 ("1.2") in the legacy field for middlebox
    // compatibility; the real version is in supported_versions.
    if (type == kExtSupportedVersions && len == 2) info.version = version_name(r.u16());
    r.seek(next);
  }
}

}  // namespace

std::string version_name(uint16_t v) {
  switch (v) {
    case 0x0300: return "SSL3.0";
    case 0x0301: return "TLS1.0";
    case 0x0302: return "TLS1.1";
    case 0x0303: return "TLS1.2";
    case 0x0304: return "TLS1.3";
    default: {
      char buf[8];
      std::snprintf(buf, sizeof buf, "0x%04x", v);
      return buf;
    }
  }
}

std::string Alert::name() const {
  if (level == 0) return "encrypted_alert";
  switch (description) {
    case 0: return "close_notify";
    case 10: return "unexpected_message";
    case 20: return "bad_record_mac";
    case 40: return "handshake_failure";
    case 42: return "bad_certificate";
    case 43: return "unsupported_certificate";
    case 44: return "certificate_revoked";
    case 45: return "certificate_expired";
    case 46: return "certificate_unknown";
    case 47: return "illegal_parameter";
    case 48: return "unknown_ca";
    case 50: return "decode_error";
    case 51: return "decrypt_error";
    case 70: return "protocol_version";
    case 71: return "insufficient_security";
    case 80: return "internal_error";
    case 86: return "inappropriate_fallback";
    case 90: return "user_canceled";
    case 109: return "missing_extension";
    case 112: return "unrecognized_name";
    case 116: return "certificate_required";
    case 120: return "no_application_protocol";
    default: return "alert_" + std::to_string(description);
  }
}

void inspect(const uint8_t* payload, size_t len, int direction, Info& info) {
  netlab::Reader r(payload, len);
  try {
    // A segment can carry several records back to back.
    while (r.remaining() >= 5) {
      uint8_t type = r.u8();
      uint16_t version = r.u16();
      uint16_t rec_len = r.u16();
      if (type < kChangeCipherSpec || type > kAppData || (version >> 8) != 3) return;  // not TLS
      if (rec_len > r.remaining()) return;  // continues in a later segment
      size_t next = r.pos() + rec_len;

      if (type == kAlert) {
        Alert a;
        a.direction = direction;
        if (rec_len == 2) {
          a.level = r.u8();
          a.description = r.u8();
        }
        if (info.alerts.size() < 16) info.alerts.push_back(a);
      } else if (type == kHandshake && !(info.client_hello && info.server_hello)) {
        uint8_t msg = r.u8();
        r.skip(3, "handshake length");
        if (msg == kClientHello && direction == 0) parse_client_hello(r, info);
        else if (msg == kServerHello && direction == 1) parse_server_hello(r, info);
      } else if (type == kAppData) {
        ++info.app_data_records;
      }
      r.seek(next);
    }
  } catch (const netlab::TruncatedError&) {
    // Partial or odd record: keep what was extracted so far.
  }
}

}  // namespace tls
