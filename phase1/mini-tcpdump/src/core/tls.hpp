#pragma once

// Reads the unencrypted parts of a TLS handshake out of TCP payloads:
// the SNI hostname in the ClientHello, the version picked in the ServerHello,
// and plaintext alerts. Everything after the handshake is encrypted, and in
// TLS 1.3 so are most alerts, so this sees what a middlebox can see, no more.
//
// No TCP reassembly: records split across segments are skipped. ClientHellos
// almost always fit in the first segment, so in practice SNI is reliable.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tls {

struct Alert {
  int direction = 0;  // 0 client->server, 1 server->client
  uint8_t level = 0;  // 1 warning, 2 fatal; 0 = encrypted (can't tell)
  uint8_t description = 0;
  std::string name() const;
};

struct Info {
  bool client_hello = false;
  bool server_hello = false;
  std::string sni;
  std::string version;  // negotiated, from the ServerHello
  uint32_t app_data_records = 0;
  std::vector<Alert> alerts;

  bool seen() const { return client_hello || server_hello || !alerts.empty(); }
};

// Feed one TCP payload. Silently ignores anything that isn't TLS.
void inspect(const uint8_t* payload, size_t len, int direction, Info& info);

std::string version_name(uint16_t v);

}  // namespace tls
