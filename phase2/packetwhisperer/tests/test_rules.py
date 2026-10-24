from pathlib import Path

import pytest
from packetwhisperer.extract import extract
from packetwhisperer.rules import diagnose_rules
from packetwhisperer.synthetic import SCENARIOS


@pytest.mark.parametrize("label", sorted(SCENARIOS))
def test_rules_recognize_each_scenario(label: str, scenario_pcaps: dict[str, Path]) -> None:
    d = diagnose_rules(extract(scenario_pcaps[label]).summary)
    assert d.label == label, d.evidence
    assert d.evidence and d.next_steps and d.summary


def test_empty_capture_is_unknown(tmp_path: Path) -> None:
    from packetwhisperer.synthetic import Capture

    d = diagnose_rules(extract(Capture().save(tmp_path / "empty.pcap")).summary)
    assert d.label == "unknown"
