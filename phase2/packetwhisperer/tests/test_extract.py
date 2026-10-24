from pathlib import Path

import pytest
from packetwhisperer.extract import compact, extract


def test_healthy_summary(scenario_pcaps: dict[str, Path]) -> None:
    ex = extract(scenario_pcaps["healthy"])
    s = ex.summary
    assert ex.packets == s["capture"]["packets"] > 100
    assert s["capture"]["decode_errors"] == 0
    (flow,) = s["tcp"]["flows"]
    assert flow["state"] == "closed"
    assert flow["retransmissions"] == [0, 0]
    assert flow["bytes_acked"][1] == 150_000 + 1  # every byte plus the FIN
    assert 19 < flow["handshake_ms"]["syn_to_synack"] < 21
    assert 19 < flow["rtt_ms"]["client_data"]["avg"] < 21  # the request, acked by the first response
    assert s["dns"]["rcodes"] == {"NOERROR": 1}
    assert "TCP flows (1)" in ex.text


def test_loss_is_visible(scenario_pcaps: dict[str, Path]) -> None:
    (flow,) = [f for f in extract(scenario_pcaps["packet_loss"]).summary["tcp"]["flows"]]
    assert flow["retransmissions"][1] >= 10
    assert flow["dup_acks"][0] >= 30


def test_compact_puts_problems_first(scenario_pcaps: dict[str, Path]) -> None:
    s = extract(scenario_pcaps["healthy"]).summary
    broken = extract(scenario_pcaps["firewall_drop"]).summary["tcp"]["flows"][0]
    s["tcp"]["flows"] = s["tcp"]["flows"] * 30 + [broken]
    out = compact(s, max_flows=5)
    assert out["tcp"]["flows"][0]["state"] == "syn_unanswered"
    assert len(out["tcp"]["flows"]) == 5
    assert out["tcp"]["flows_not_shown"] == 26
    assert len(s["tcp"]["flows"]) == 31  # input untouched


def test_unreadable_file_raises(tmp_path: Path) -> None:
    junk = tmp_path / "junk.pcap"
    junk.write_bytes(b"definitely not a pcap")
    with pytest.raises(RuntimeError):
        extract(junk)
