#pragma once

// Probe format, all big-endian:
//   0  magic   u32  'UPNG'
//   4  seq     u32  probe number, starting at 0
//   8  sent_ns u64  client's monotonic clock at send time, echoed back untouched
//  16  padding      to the requested probe size
//
// The server echoes the datagram verbatim, so the client computes RTT from the
// echoed timestamp and needs no per-probe bookkeeping (and clock skew between
// hosts doesn't matter: only the client's clock is ever read).

#include <cstddef>
#include <cstdint>

namespace udpping {

constexpr uint32_t kMagic = 0x55504E47;  // "UPNG"
constexpr size_t kHeaderSize = 16;

struct Probe {
  uint32_t seq = 0;
  uint64_t sent_ns = 0;
};

inline void encode(const Probe& p, uint8_t* out) {
  auto put = [&out](uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) *out++ = static_cast<uint8_t>(v >> (8 * i));
  };
  put(kMagic, 4);
  put(p.seq, 4);
  put(p.sent_ns, 8);
}

inline bool decode(const uint8_t* in, size_t len, Probe& p) {
  if (len < kHeaderSize) return false;
  auto get = [&in](int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | *in++;
    return v;
  };
  if (get(4) != kMagic) return false;
  p.seq = static_cast<uint32_t>(get(4));
  p.sent_ns = get(8);
  return true;
}

}  // namespace udpping
