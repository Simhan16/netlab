from pathlib import Path

import pytest
from packetwhisperer.synthetic import SCENARIOS, write_scenario


@pytest.fixture(scope="session")
def scenario_pcaps(tmp_path_factory: pytest.TempPathFactory) -> dict[str, Path]:
    out = tmp_path_factory.mktemp("pcaps")
    return {label: write_scenario(label, out / f"{label}.pcap") for label in SCENARIOS}
