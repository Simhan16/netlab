#!/usr/bin/env python3
"""Write one synthetic pcap per scenario plus a labels.json, for smoke-testing the eval pipeline."""

import json
from pathlib import Path

from packetwhisperer.synthetic import SCENARIOS, write_scenario

OUT = Path(__file__).resolve().parent / "synthetic"


def main() -> None:
    OUT.mkdir(exist_ok=True)
    captures = []
    for label in SCENARIOS:
        write_scenario(label, OUT / f"{label}.pcap")
        captures.append({"id": f"synthetic-{label}", "pcap": f"{label}.pcap", "label": label, "case": label,
                         "capture_point": "synthetic", "synthetic": True,
                         "description": "hand-built textbook pattern (packetwhisperer.synthetic)"})
    (OUT / "labels.json").write_text(json.dumps({"version": 1, "captures": captures}, indent=2) + "\n")
    print(f"wrote {len(captures)} synthetic captures to {OUT}")


if __name__ == "__main__":
    main()
