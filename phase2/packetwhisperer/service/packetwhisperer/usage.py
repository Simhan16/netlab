"""Append-only usage log (JSON lines) and the report built from it.

This is where the case study's numbers come from: how many captures were
diagnosed, for whom, what was found, and how long it took. Never fill those
numbers in by hand.
"""

from __future__ import annotations

import json
import statistics
import threading
import time
from collections import Counter
from pathlib import Path
from typing import Any


class UsageLog:
    def __init__(self, path: Path) -> None:
        self.path = path
        self._lock = threading.Lock()

    def record(self, **fields: Any) -> None:
        entry = {"ts": round(time.time(), 3), **fields}
        line = json.dumps(entry, sort_keys=True) + "\n"
        with self._lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.path.open("a") as f:
                f.write(line)


def read_entries(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    with path.open() as f:
        return [json.loads(line) for line in f if line.strip()]


def summarize(entries: list[dict[str, Any]]) -> dict[str, Any]:
    ok = [e for e in entries if e.get("status") == "ok"]
    latencies = sorted(e["latency_ms"] for e in ok if "latency_ms" in e)

    def pct(p: float) -> float | None:
        return latencies[min(len(latencies) - 1, int(p * len(latencies)))] if latencies else None

    return {
        "requests": len(entries),
        "diagnosed": len(ok),
        "errors": len(entries) - len(ok),
        "callers": len({e.get("caller") for e in entries}),
        "labels": dict(Counter(e.get("label") for e in ok).most_common()),
        "engines": dict(Counter(e.get("engine") for e in ok)),
        "latency_ms_p50": pct(0.50),
        "latency_ms_p95": pct(0.95),
        "latency_ms_mean": round(statistics.fmean(latencies), 1) if latencies else None,
        "input_tokens": sum(e.get("input_tokens", 0) for e in ok),
        "output_tokens": sum(e.get("output_tokens", 0) for e in ok),
        "first_ts": min((e["ts"] for e in entries), default=None),
        "last_ts": max((e["ts"] for e in entries), default=None),
    }
