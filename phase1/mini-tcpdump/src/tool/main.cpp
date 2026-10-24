// mini-tcpdump: capture or read packets, print them, and summarize every flow.
//
//   mini-tcpdump -i <iface> [-w out.pcap] [-c count] [-s snaplen] [-q] [--json file] [filter...]
//   mini-tcpdump -r in.pcap [-q] [--json file] [filter...]
//   mini-tcpdump -D                     list interfaces
//
// Filters are BPF, same syntax as tcpdump: 'tcp port 443', 'host 1.1.1.1 and udp'.
// BPF runs in the kernel, so filtered-out packets never even reach this process.

#include <pcap/pcap.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "analyzer.hpp"

namespace {

pcap_t* g_handle = nullptr;
void on_signal(int) {
  if (g_handle) pcap_breakloop(g_handle);  // async-signal-safe per libpcap docs
}

struct Options {
  std::string interface, read_file, write_file, json_file, filter;
  long count = -1;
  int snaplen = 262144;
  bool quiet = false;
  bool list = false;
};

struct Context {
  pkt::Analyzer* analyzer;
  pcap_dumper_t* dumper;
  bool quiet;
};

void on_packet(u_char* user, const pcap_pkthdr* h, const u_char* data) {
  auto* ctx = reinterpret_cast<Context*>(user);
  if (ctx->dumper) pcap_dump(reinterpret_cast<u_char*>(ctx->dumper), h, data);
  double ts = static_cast<double>(h->ts.tv_sec) + static_cast<double>(h->ts.tv_usec) / 1e6;
  bool ok = ctx->analyzer->feed(ts, data, h->caplen, h->len);
  if (!ctx->quiet) {
    if (ok) std::puts(pkt::describe(ctx->analyzer->last_packet()).c_str());
    else std::printf("%.6f [undecodable %u-byte frame]\n", ts, h->len);
  }
}

Options parse_args(int argc, char** argv) {
  Options o;
  auto need = [&](int& i) -> std::string {
    if (i + 1 >= argc) throw std::invalid_argument(std::string(argv[i]) + " needs a value");
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-i") o.interface = need(i);
    else if (a == "-r") o.read_file = need(i);
    else if (a == "-w") o.write_file = need(i);
    else if (a == "-c") o.count = std::stol(need(i));
    else if (a == "-s") o.snaplen = std::stoi(need(i));
    else if (a == "--json") o.json_file = need(i);
    else if (a == "-q") o.quiet = true;
    else if (a == "-D") o.list = true;
    else if (a == "-h" || a == "--help") throw std::invalid_argument("");
    else o.filter += (o.filter.empty() ? "" : " ") + a;
  }
  if (!o.list && o.interface.empty() == o.read_file.empty()) {
    throw std::invalid_argument("give exactly one of -i <iface> or -r <file>");
  }
  return o;
}

int list_interfaces() {
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_if_t* devs = nullptr;
  if (pcap_findalldevs(&devs, errbuf) != 0) throw std::runtime_error(errbuf);
  int n = 0;
  for (pcap_if_t* d = devs; d; d = d->next) {
    std::printf("%d.%s%s%s\n", ++n, d->name, d->description ? " " : "", d->description ? d->description : "");
  }
  pcap_freealldevs(devs);
  return 0;
}

pcap_t* open_live(const Options& o) {
  char errbuf[PCAP_ERRBUF_SIZE] = {};
  pcap_t* h = pcap_create(o.interface.c_str(), errbuf);
  if (!h) throw std::runtime_error(errbuf);
  pcap_set_snaplen(h, o.snaplen);
  pcap_set_promisc(h, 1);
  // Immediate mode: deliver each packet as it arrives instead of batching,
  // which matters for watching low-rate traffic like a handshake.
  pcap_set_immediate_mode(h, 1);
  int rc = pcap_activate(h);
  if (rc < 0) {
    std::string err = pcap_geterr(h);
    pcap_close(h);
    throw std::runtime_error("activate " + o.interface + ": " + err + (rc == PCAP_ERROR_PERM_DENIED ? " (try sudo)" : ""));
  }
  return h;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  try {
    o = parse_args(argc, argv);
  } catch (const std::exception& e) {
    if (*e.what()) std::fprintf(stderr, "mini-tcpdump: %s\n", e.what());
    std::fprintf(stderr,
                 "usage: mini-tcpdump (-i iface | -r file) [-w out.pcap] [-c count] [-s snaplen] [-q]\n"
                 "                    [--json summary.json] [bpf filter...]\n"
                 "       mini-tcpdump -D\n");
    return 2;
  }

  try {
    if (o.list) return list_interfaces();

    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_t* raw = o.read_file.empty() ? open_live(o) : pcap_open_offline(o.read_file.c_str(), errbuf);
    if (!raw) throw std::runtime_error(errbuf);
    std::unique_ptr<pcap_t, decltype(&pcap_close)> handle(raw, &pcap_close);

    if (!o.filter.empty()) {
      bpf_program prog{};
      if (pcap_compile(handle.get(), &prog, o.filter.c_str(), 1, PCAP_NETMASK_UNKNOWN) != 0 ||
          pcap_setfilter(handle.get(), &prog) != 0) {
        std::string err = pcap_geterr(handle.get());
        pcap_freecode(&prog);
        throw std::runtime_error("filter '" + o.filter + "': " + err);
      }
      pcap_freecode(&prog);
    }

    std::unique_ptr<pcap_dumper_t, decltype(&pcap_dump_close)> dumper(nullptr, &pcap_dump_close);
    if (!o.write_file.empty()) {
      dumper.reset(pcap_dump_open(handle.get(), o.write_file.c_str()));
      if (!dumper) throw std::runtime_error("write " + o.write_file + ": " + pcap_geterr(handle.get()));
    }

    pkt::Analyzer analyzer(pcap_datalink(handle.get()));
    Context ctx{&analyzer, dumper.get(), o.quiet};

    g_handle = handle.get();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    if (!o.interface.empty()) {
      std::fprintf(stderr, "listening on %s, link-type %s, snaplen %d\n", o.interface.c_str(),
                   pkt::link_name(pcap_datalink(handle.get())).c_str(), o.snaplen);
    }

    auto start = std::chrono::steady_clock::now();
    int rc = pcap_loop(handle.get(), static_cast<int>(o.count), on_packet, reinterpret_cast<u_char*>(&ctx));
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    g_handle = nullptr;
    if (rc == PCAP_ERROR) throw std::runtime_error(pcap_geterr(handle.get()));
    if (dumper) pcap_dump_flush(dumper.get());

    std::printf("\n%s", analyzer.summary_text().c_str());
    if (!o.read_file.empty() && elapsed > 0) {
      std::printf("\nanalyzed %llu packets in %.3f s (%.0f packets/s)\n",
                  static_cast<unsigned long long>(analyzer.packets()), elapsed,
                  static_cast<double>(analyzer.packets()) / elapsed);
    }
    if (!o.interface.empty()) {
      pcap_stat st{};
      if (pcap_stats(handle.get(), &st) == 0) {
        // Kernel drops mean the capture missed packets: never mistake them for network loss.
        std::fprintf(stderr, "%u packets received by filter, %u dropped by kernel\n", st.ps_recv, st.ps_drop);
      }
    }
    if (!o.json_file.empty()) {
      std::ofstream(o.json_file) << analyzer.summary_json() << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "mini-tcpdump: %s\n", e.what());
    return 1;
  }
}
