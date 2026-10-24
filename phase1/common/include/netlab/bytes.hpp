#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace netlab {

// Thrown when a packet is shorter than its headers claim. Packet parsers see
// hostile and truncated input all the time; every read must be bounds-checked.
struct TruncatedError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Cursor over a byte buffer that reads big-endian ("network byte order") integers.
class Reader {
 public:
  Reader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

  size_t pos() const { return pos_; }
  size_t size() const { return len_; }
  size_t remaining() const { return len_ - pos_; }
  const uint8_t* data() const { return data_; }
  const uint8_t* cursor() const { return data_ + pos_; }

  void need(size_t n, const char* what) const {
    if (n > remaining()) {
      throw TruncatedError(std::string("truncated ") + what + ": need " + std::to_string(n) +
                           " bytes, have " + std::to_string(remaining()));
    }
  }
  void seek(size_t pos) {
    if (pos > len_) throw TruncatedError("seek past end");
    pos_ = pos;
  }
  void skip(size_t n, const char* what = "field") {
    need(n, what);
    pos_ += n;
  }

  uint8_t u8() {
    need(1, "u8");
    return data_[pos_++];
  }
  uint16_t u16() {
    need(2, "u16");
    uint16_t v = static_cast<uint16_t>(data_[pos_] << 8 | data_[pos_ + 1]);
    pos_ += 2;
    return v;
  }
  uint32_t u32() {
    need(4, "u32");
    uint32_t v = static_cast<uint32_t>(data_[pos_]) << 24 | static_cast<uint32_t>(data_[pos_ + 1]) << 16 |
                 static_cast<uint32_t>(data_[pos_ + 2]) << 8 | static_cast<uint32_t>(data_[pos_ + 3]);
    pos_ += 4;
    return v;
  }

 private:
  const uint8_t* data_;
  size_t len_;
  size_t pos_ = 0;
};

}  // namespace netlab
