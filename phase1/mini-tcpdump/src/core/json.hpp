#pragma once

// Minimal streaming JSON writer: enough for the analyzer's summary, no deps.

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pkt {

class Json {
 public:
  Json& begin_object() { return open('{'); }
  Json& end_object() { return close('}'); }
  Json& begin_array() { return open('['); }
  Json& end_array() { return close(']'); }

  Json& key(std::string_view k) {
    separate();
    quote(k);
    out_ += ':';
    after_key_ = true;
    return *this;
  }

  Json& value(std::string_view s) {
    separate();
    quote(s);
    return *this;
  }
  Json& value(const char* s) { return value(std::string_view(s)); }
  Json& value(bool b) {
    separate();
    out_ += b ? "true" : "false";
    return *this;
  }
  Json& value(double d) {
    separate();
    if (!std::isfinite(d)) {
      out_ += "null";
      return *this;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6f", d);
    std::string s = buf;
    s.erase(s.find_last_not_of('0') + 1);  // 12.500000 -> 12.5
    if (s.back() == '.') s.pop_back();
    out_ += s;
    return *this;
  }
  template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
  Json& value(T v) {
    separate();
    out_ += std::to_string(v);
    return *this;
  }
  Json& null() {
    separate();
    out_ += "null";
    return *this;
  }

  template <typename T>
  Json& field(std::string_view k, const T& v) {
    key(k);
    return value(v);
  }

  const std::string& str() const { return out_; }

 private:
  Json& open(char c) {
    separate();
    out_ += c;
    first_.push_back(true);
    return *this;
  }
  Json& close(char c) {
    out_ += c;
    first_.pop_back();
    return *this;
  }
  void separate() {
    if (after_key_) {
      after_key_ = false;
      return;
    }
    if (!first_.empty()) {
      if (!first_.back()) out_ += ',';
      first_.back() = false;
    }
  }
  void quote(std::string_view s) {
    out_ += '"';
    for (char c : s) {
      unsigned char u = static_cast<unsigned char>(c);
      if (c == '"' || c == '\\') {
        out_ += '\\';
        out_ += c;
      } else if (u < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", u);
        out_ += buf;
      } else {
        out_ += c;
      }
    }
    out_ += '"';
  }

  std::string out_;
  std::vector<bool> first_;
  bool after_key_ = false;
};

}  // namespace pkt
