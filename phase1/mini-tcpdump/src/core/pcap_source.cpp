#include "pcap_source.hpp"

#include <pcap/pcap.h>

#include <chrono>
#include <memory>
#include <stdexcept>

#include "analyzer.hpp"

namespace pkt {

FileAnalysis analyze_file(const std::string& path) {
  char errbuf[PCAP_ERRBUF_SIZE] = {};
  std::unique_ptr<pcap_t, decltype(&pcap_close)> handle(pcap_open_offline(path.c_str(), errbuf), &pcap_close);
  if (!handle) throw std::runtime_error("open " + path + ": " + errbuf);

  auto start = std::chrono::steady_clock::now();
  Analyzer analyzer(pcap_datalink(handle.get()));
  pcap_pkthdr* header = nullptr;
  const u_char* data = nullptr;
  int rc;
  while ((rc = pcap_next_ex(handle.get(), &header, &data)) == 1) {
    double ts = static_cast<double>(header->ts.tv_sec) + static_cast<double>(header->ts.tv_usec) / 1e6;
    analyzer.feed(ts, data, header->caplen, header->len);
  }
  if (rc == PCAP_ERROR) throw std::runtime_error("read " + path + ": " + pcap_geterr(handle.get()));

  FileAnalysis result;
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  result.packets = analyzer.packets();
  result.summary_json = analyzer.summary_json();
  result.summary_text = analyzer.summary_text();
  return result;
}

}  // namespace pkt
