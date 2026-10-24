// Python binding for pktcore. Deliberately thin: one call, plain data out.
// The summary crosses the boundary as a JSON string (parsed on the Python
// side), so the C++ and Python halves share one schema and no wrapper classes.

#include <pybind11/pybind11.h>

#include <string>

#include "pcap_source.hpp"

namespace py = pybind11;

PYBIND11_MODULE(_pktcore, m) {
  m.doc() = "pcap decoding and flow analysis (mini-tcpdump's pktcore)";

  m.def(
      "analyze_file",
      [](const std::string& path) {
        pkt::FileAnalysis result;
        {
          // Parsing is pure C++: release the GIL so other Python threads
          // (e.g. concurrent API requests) keep running meanwhile.
          py::gil_scoped_release release;
          result = pkt::analyze_file(path);
        }
        py::dict out;
        out["summary_json"] = result.summary_json;
        out["summary_text"] = result.summary_text;
        out["packets"] = result.packets;
        out["seconds"] = result.seconds;
        return out;
      },
      py::arg("path"),
      "Analyze a pcap file. Returns {summary_json, summary_text, packets, seconds}. "
      "Raises RuntimeError if the file can't be read.");
}
