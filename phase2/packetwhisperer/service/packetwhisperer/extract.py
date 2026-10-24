"""Extraction layer: pcap -> structured flow summary, via the C++ parser.

The LLM never sees raw packets. It sees the summary mini-tcpdump computes
(flow states, retransmissions, RTTs, DNS outcomes, ICMP errors), trimmed so
the suspicious parts survive and the bulk doesn't blow the prompt budget.
"""

from __future__ import annotations

import copy
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# States where nothing obviously went wrong with the connection itself.
UNREMARKABLE_STATES = {"open", "closed", "half_closed", "midstream", "midstream_closed"}


@dataclass(frozen=True)
class Extraction:
    summary: dict[str, Any]
    text: str
    packets: int
    seconds: float

    @property
    def packets_per_second(self) -> float:
        return self.packets / self.seconds if self.seconds > 0 else 0.0


def extract(pcap: str | Path) -> Extraction:
    """Run the C++ analyzer over a pcap file. Raises RuntimeError on unreadable files."""
    # The native module is imported lazily so pure-Python code (rules, tests' fakes) loads without it.
    from packetwhisperer import _pktcore

    raw = _pktcore.analyze_file(str(pcap))
    return Extraction(
        summary=json.loads(raw["summary_json"]),
        text=raw["summary_text"],
        packets=raw["packets"],
        seconds=raw["seconds"],
    )


def flow_interest(flow: dict[str, Any]) -> float:
    """How much a TCP flow deserves the reader's attention. Problems first, then size."""
    score = 0.0
    if flow["state"] not in UNREMARKABLE_STATES:
        score += 100
    score += 10 * sum(flow["retransmissions"])
    score += 5 * max(0, flow["syns"] - 1) + 5 * max(0, flow["synacks"] - 1)
    score += 3 * sum(flow["dup_acks"]) + 3 * sum(flow["zero_windows"]) + 5 * sum(flow["rsts"])
    if flow.get("tls", {}).get("alerts"):
        score += 50
    return score + math.log1p(sum(flow["packets"]))


def compact(summary: dict[str, Any], max_flows: int = 20, max_udp_flows: int = 10) -> dict[str, Any]:
    """Trim a summary for a prompt: keep every aggregate, the most interesting flows, and say what was cut."""
    out = copy.deepcopy(summary)
    tcp = out.get("tcp", {})
    flows = sorted(tcp.get("flows", []), key=flow_interest, reverse=True)
    tcp["flows"] = flows[:max_flows]
    tcp["flows_not_shown"] = max(0, len(flows) - max_flows) + tcp.pop("flows_omitted", 0)

    udp = out.get("udp", {})
    udp_flows = udp.get("flows", [])
    udp["flows"] = udp_flows[:max_udp_flows]
    udp["flows_not_shown"] = max(0, udp.get("flow_count", len(udp_flows)) - max_udp_flows)
    return out
