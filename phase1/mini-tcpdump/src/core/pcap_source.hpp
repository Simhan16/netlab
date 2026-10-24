#pragma once

// The only part of pktcore that touches libpcap: reading capture files.

#include <cstdint>
#include <string>

namespace pkt {

struct FileAnalysis {
  std::string summary_json;
  std::string summary_text;
  uint64_t packets = 0;
  double seconds = 0;  // wall-clock analysis time

  double packets_per_second() const { return seconds > 0 ? static_cast<double>(packets) / seconds : 0; }
};

// Throws std::runtime_error if the file can't be opened or read.
FileAnalysis analyze_file(const std::string& path);

}  // namespace pkt
