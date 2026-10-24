"""Heuristic diagnoser: hand-written rules over the flow summary.

Three jobs: a baseline the LLM must beat in the evals (if rules score the
same, the LLM isn't earning its cost), a fallback when the API is down, and a
way to test the whole pipeline without an API key.

Each rule looks for one failure signature and returns a score in [0, 1];
the highest-scoring rule wins. The thresholds match the casebook's lab
conditions, so expect them to need tuning on real-world captures.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from packetwhisperer.models import Diagnosis
from packetwhisperer.taxonomy import LABELS, NEXT_STEPS

Summary = dict[str, Any]


@dataclass
class Finding:
    label: str
    score: float
    evidence: list[str] = field(default_factory=list)


def _flows(s: Summary) -> list[dict[str, Any]]:
    return s.get("tcp", {}).get("flows", [])


def _pair(flow: dict[str, Any]) -> str:
    return f"{flow['client']} -> {flow['server']}"


def rule_tls(s: Summary) -> Finding | None:
    for f in _flows(s):
        tls = f.get("tls")
        if not tls:
            continue
        fatal = [a for a in tls.get("alerts", []) if a["level"] == "fatal"]
        if fatal and tls.get("app_data_records", 0) == 0:
            a = fatal[0]
            sni = f" for SNI {tls['sni']}" if tls.get("sni") else ""
            return Finding("tls_handshake_failure", 0.9, [
                f"TLS handshake on {_pair(f)}{sni} ended with a fatal {a['description']} alert from the {a['from']}",
                "No application data was exchanged after the alert",
            ])
    return None


def rule_dns_unreachable(s: Summary) -> Finding | None:
    dns = s.get("dns", {})
    unanswered = dns.get("unanswered", [])
    attempts = sum(u["attempts"] for u in unanswered)
    if attempts < 2:
        return None
    servers = sorted({u["server"] for u in unanswered})
    names = sorted({u["name"] for u in unanswered})
    evidence = [f"{attempts} DNS queries to {', '.join(servers)} got no response (names: {', '.join(names[:3])})"]
    if dns.get("responses", 0) == 0:
        evidence.append("Not a single DNS response appears in the capture")
        if not _flows(s):
            evidence.append("No TCP connection was attempted, consistent with the app failing at name lookup")
        return Finding("dns_unreachable", 0.85, evidence)
    return Finding("dns_unreachable", 0.45, evidence)


def rule_dns_nxdomain(s: Summary) -> Finding | None:
    failures = [f for f in s.get("dns", {}).get("failures", []) if f["rcode"] == "NXDOMAIN"]
    if not failures:
        return None
    evidence = [f"DNS answered NXDOMAIN for {f['name']} ({f['qtype']}, x{f['count']})" for f in failures[:3]]
    useful_tcp = [f for f in _flows(s) if f["payload_bytes"][1] > 0]
    if not useful_tcp:
        evidence.append("No TCP connection carried data, consistent with the app stopping at the failed lookup")
        return Finding("dns_nxdomain", 0.85, evidence)
    return Finding("dns_nxdomain", 0.3, evidence)  # lookups failing alongside working traffic: likely noise


def rule_refused(s: Summary) -> Finding | None:
    refused = [f for f in _flows(s) if f["state"] == "refused"]
    if not refused:
        return None
    f = refused[0]
    return Finding("connection_refused", 0.85, [
        f"{len(refused)} connection attempt(s) answered with RST, e.g. {_pair(f)}",
        "The server responded immediately, so the path works but nothing is listening on that port",
    ])


def rule_firewall_drop(s: Summary) -> Finding | None:
    dropped = [f for f in _flows(s) if f["state"] == "syn_unanswered" and f["syns"] >= 2]
    if not dropped:
        return None
    f = dropped[0]
    evidence = [
        f"{_pair(f)}: {f['syns']} SYNs over {f['duration_s']:.1f}s with no reply at all (no SYN-ACK, no RST)",
    ]
    if not s.get("icmp", {}).get("errors"):
        evidence.append("No ICMP errors either: the packets are being silently discarded")
    return Finding("firewall_drop", 0.8, evidence)


def rule_asymmetric(s: Summary) -> Finding | None:
    stuck = [f for f in _flows(s) if f["state"] == "synack_unacked" and f["synacks"] >= 2]
    if not stuck:
        return None
    f = stuck[0]
    return Finding("asymmetric_routing", 0.8, [
        f"{_pair(f)}: server sent {f['synacks']} SYN-ACKs in reply to {f['syns']} SYNs, but the client never "
        "completed the handshake",
        "The client keeps retrying as if it never received the SYN-ACKs: replies are lost on the return path",
    ])


def rule_mtu(s: Summary) -> Finding | None:
    frag_kinds = ("dest-unreach/frag-needed", "packet-too-big")
    frag = [e for e in s.get("icmp", {}).get("errors", []) if e["kind"] in frag_kinds]
    for f in _flows(s):
        if f["synacks"] == 0 or f["state"] in ("syn_unanswered", "refused"):
            continue
        retrans, max_payload, acked = f["retransmissions"][1], f["max_payload"][1], f["bytes_acked"][1]
        if retrans >= 3 and max_payload >= 1000 and acked < 4 * max_payload:
            evidence = [
                f"{_pair(f)}: handshake succeeded, but the server retransmitted {retrans} full-size segments "
                f"(up to {max_payload} bytes) that were never acknowledged",
                f"Only {acked} bytes from the server were ever acknowledged: "
                "small packets get through, large ones don't",
            ]
            if not frag:
                evidence.append("No ICMP 'fragmentation needed' messages seen, so Path MTU Discovery can't recover")
            return Finding("mtu_blackhole", 0.8, evidence)
    if frag:
        e = frag[0]
        return Finding("mtu_blackhole", 0.4, [f"ICMP {e['kind']} from {e['from']} (MTU {e.get('mtu', '?')})"])
    return None


def rule_loss(s: Summary) -> Finding | None:
    tcp = s.get("tcp", {})
    packets, retrans = tcp.get("packets", 0), tcp.get("retransmissions", 0)
    if packets < 50 or retrans / packets < 0.02:
        return None
    progressed = [f for f in _flows(s) if max(f["bytes_acked"]) > 50_000]
    if not progressed:
        return None
    dup_acks = sum(sum(f["dup_acks"]) for f in _flows(s))
    return Finding("packet_loss", 0.75, [
        f"{retrans} retransmissions across {packets} TCP packets ({100 * retrans / packets:.1f}%)",
        f"{dup_acks} duplicate ACKs: receivers repeatedly reported gaps",
        f"Transfers still made progress ({max(max(f['bytes_acked']) for f in progressed)} bytes acknowledged), "
        "so packets are being lost, not blocked",
    ])


def rule_latency(s: Summary) -> Finding | None:
    tcp = s.get("tcp", {})
    rtts: list[tuple[float, str]] = []
    for f in _flows(s):
        hs = f.get("handshake_ms", {})
        total = hs.get("syn_to_synack", 0) + hs.get("synack_to_ack", 0)
        if total:
            rtts.append((total, f"handshake on {_pair(f)} took {total:.0f} ms"))
        for side, st in f.get("rtt_ms", {}).items():
            who = side.split("_")[0]
            rtts.append((st["avg"], f"{who} data on {_pair(f)} took {st['avg']:.0f} ms on average to be acknowledged"))
    if not rtts:
        return None
    worst = max(rtts)
    loss = tcp.get("retransmissions", 0) / max(1, tcp.get("packets", 0))
    if worst[0] < 150 or loss > 0.02:
        return None
    return Finding("high_latency", 0.7, [worst[1], f"Retransmission rate is only {100 * loss:.1f}%: slow, not lossy"])


RULES: list[Callable[[Summary], Finding | None]] = [
    rule_tls, rule_dns_unreachable, rule_dns_nxdomain, rule_refused, rule_firewall_drop,
    rule_asymmetric, rule_mtu, rule_loss, rule_latency,
]

_SUMMARIES = {
    "healthy": "The capture looks normal: connections complete and DNS answers promptly.",
    "dns_unreachable": "The device can't reach its DNS server, so it can't look up names and never connects.",
    "dns_nxdomain": "The name the device is looking up doesn't exist in DNS, so it doesn't know where to connect.",
    "mtu_blackhole": (
        "Small packets get through but large ones are silently dropped somewhere on the path, so transfers "
        "stall after connecting."
    ),
    "tls_handshake_failure": (
        "The connection opens, but the secure (TLS) handshake fails, usually a certificate problem."
    ),
    "asymmetric_routing": "Requests reach the server, but its replies take a different path and get lost.",
    "firewall_drop": "Something on the path, usually a firewall, silently drops the connection attempts.",
    "connection_refused": "The server is reachable but nothing is running on that port.",
    "packet_loss": "The network is losing packets; things work but slowly and unreliably.",
    "high_latency": "The network works but every round trip is slow.",
    "unknown": "The capture doesn't show enough to pinpoint a cause.",
}


def diagnose_rules(summary: Summary) -> Diagnosis:
    findings = [f for rule in RULES if (f := rule(summary)) is not None]
    best = max(findings, key=lambda f: f.score, default=None)

    if best is None or best.score < 0.5:
        if summary.get("capture", {}).get("packets", 0) == 0:
            best = Finding("unknown", 0.0, ["The capture contains no packets"])
        elif _flows(summary) or summary.get("dns", {}).get("responses"):
            weak = [e for f in findings for e in f.evidence]
            minor = [f"Minor: {e}" for e in weak]
            best = Finding("healthy", 0.6, ["Connections and lookups completed without notable errors", *minor])
        else:
            best = Finding("unknown", 0.2, ["No TCP connections or DNS answers to judge by"])

    confidence = "high" if best.score >= 0.8 else "medium" if best.score >= 0.6 else "low"
    assert best.label in LABELS
    return Diagnosis(
        label=best.label,
        confidence=confidence,
        summary=_SUMMARIES[best.label],
        evidence=best.evidence,
        next_steps=NEXT_STEPS[best.label],
        engine="rules",
    )
