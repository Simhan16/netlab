#!/usr/bin/env python3
"""Score diagnosers against labeled captures: accuracy overall and per failure type.

    python evals/run_evals.py                                  # rules engine on the real casebook
    python evals/run_evals.py --engine rules llm --yes         # compare; llm calls the API (costs money)
    python evals/run_evals.py --labels evals/synthetic/labels.json   # plumbing smoke test only

Results go to evals/results/<timestamp>-<engine>.{json,md}. Quote numbers from
real-casebook runs only; synthetic runs measure nothing about real traffic.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from packetwhisperer.diagnose import diagnose
from packetwhisperer.extract import Extraction, extract
from packetwhisperer.llm import LLMBadResponse, LLMUnavailable
from packetwhisperer.taxonomy import LABELS

HERE = Path(__file__).resolve().parent
DEFAULT_LABELS = HERE.parents[2] / "phase1" / "casebook" / "labels.json"

# USD per million tokens (input, output). Verify against current pricing before quoting costs.
PRICES_PER_MTOK = {"claude-opus-5-5": (4.00, 20.00)}


@dataclass
class Result:
    id: str
    truth: str
    predicted: str | None
    correct: bool
    confidence: str | None
    engine_used: str | None
    latency_s: float
    input_tokens: int = 0
    output_tokens: int = 0
    model: str | None = None
    error: str | None = None


def load_captures(labels_path: Path) -> tuple[list[dict[str, Any]], bool]:
    data = json.loads(labels_path.read_text())
    captures = data.get("captures", [])
    for c in captures:
        c["pcap_path"] = (labels_path.parent / c["pcap"]).resolve()
        if c["label"] not in LABELS:
            raise SystemExit(f"{c['id']}: label {c['label']!r} is not in taxonomy.py")
    synthetic = any(c.get("synthetic") for c in captures)
    return captures, synthetic


def run_engine(engine: str, captures: list[dict[str, Any]], extractions: dict[str, Extraction]) -> list[Result]:
    results = []
    for c in captures:
        t0 = time.perf_counter()
        try:
            d = diagnose(extractions[c["id"]].summary, engine)  # type: ignore[arg-type]
        except (LLMUnavailable, LLMBadResponse) as e:
            results.append(Result(c["id"], c["label"], None, False, None, None, time.perf_counter() - t0, error=str(e)))
            print(f"  {c['id']:<36} ERROR {e}", file=sys.stderr)
            continue
        latency = time.perf_counter() - t0
        r = Result(
            c["id"], c["label"], d.label, d.label == c["label"], d.confidence, d.engine, round(latency, 3),
            d.usage.input_tokens if d.usage else 0, d.usage.output_tokens if d.usage else 0,
            d.usage.model if d.usage else None,
        )
        results.append(r)
        mark = "ok " if r.correct else "BAD"
        print(f"  {mark} {c['id']:<36} truth={c['label']:<22} got={d.label} ({d.confidence})", file=sys.stderr)
    return results


def score(results: list[Result]) -> dict[str, Any]:
    per_label: dict[str, Counter[str]] = defaultdict(Counter)
    confusion: dict[str, Counter[str]] = defaultdict(Counter)
    for r in results:
        per_label[r.truth]["n"] += 1
        per_label[r.truth]["correct"] += r.correct
        confusion[r.truth][r.predicted or "error"] += 1
    cost = 0.0
    for r in results:
        price_in, price_out = PRICES_PER_MTOK.get(r.model or "", (0.0, 0.0))
        cost += r.input_tokens / 1e6 * price_in + r.output_tokens / 1e6 * price_out
    n = len(results)
    return {
        "n": n,
        "accuracy": sum(r.correct for r in results) / n if n else 0.0,
        "errors": sum(r.error is not None for r in results),
        "per_label": {k: {"n": v["n"], "accuracy": v["correct"] / v["n"]} for k, v in sorted(per_label.items())},
        "confusion": {k: dict(v) for k, v in sorted(confusion.items())},
        "mean_latency_s": sum(r.latency_s for r in results) / n if n else 0.0,
        "input_tokens": sum(r.input_tokens for r in results),
        "output_tokens": sum(r.output_tokens for r in results),
        "estimated_cost_usd": round(cost, 4),
    }


def markdown(engine: str, labels_path: Path, synthetic: bool, s: dict[str, Any]) -> str:
    warning = " **(SYNTHETIC: plumbing check, not a real accuracy number)**" if synthetic else ""
    lines = [
        f"# Eval: {engine} engine",
        "",
        f"- labels: `{labels_path}`{warning}",
        f"- captures: {s['n']}, errors: {s['errors']}",
        f"- **accuracy: {100 * s['accuracy']:.1f}%**",
        f"- mean latency: {s['mean_latency_s']:.2f}s, tokens in/out: {s['input_tokens']}/{s['output_tokens']}, "
        f"est. cost ${s['estimated_cost_usd']}",
        "",
        "| label | n | accuracy | predicted as |",
        "|---|---|---|---|",
    ]
    for label, v in s["per_label"].items():
        preds = ", ".join(f"{p}×{c}" for p, c in sorted(s["confusion"][label].items(), key=lambda kv: -kv[1]))
        lines.append(f"| {label} | {v['n']} | {100 * v['accuracy']:.0f}% | {preds} |")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--labels", type=Path, default=DEFAULT_LABELS)
    parser.add_argument("--engine", nargs="+", choices=["rules", "llm"], default=["rules"])
    parser.add_argument("--limit", type=int, help="only the first N captures")
    parser.add_argument("--yes", action="store_true", help="confirm API spend for the llm engine")
    parser.add_argument("--out", type=Path, default=HERE / "results")
    args = parser.parse_args()

    captures, synthetic = load_captures(args.labels)
    if args.limit:
        captures = captures[: args.limit]
    if not captures:
        print(f"{args.labels} has no captures yet. Run the casebook first:\n"
              "  sudo phase1/casebook/run_case.sh all 3\n"
              "or smoke-test the plumbing with:  python evals/make_synthetic.py", file=sys.stderr)
        return 1
    if "llm" in args.engine and not args.yes:
        print(f"The llm engine makes {len(captures)} API calls (~1 per capture). Re-run with --yes to proceed.",
              file=sys.stderr)
        return 1

    print(f"extracting {len(captures)} captures...", file=sys.stderr)
    extractions = {c["id"]: extract(c["pcap_path"]) for c in captures}

    args.out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    for engine in args.engine:
        print(f"\n[{engine}]", file=sys.stderr)
        results = run_engine(engine, captures, extractions)
        s = score(results)
        md = markdown(engine, args.labels, synthetic, s)
        base = args.out / f"{stamp}-{engine}{'-synthetic' if synthetic else ''}"
        base.with_suffix(".json").write_text(json.dumps(
            {"engine": engine, "labels": str(args.labels), "synthetic": synthetic, "score": s,
             "results": [asdict(r) for r in results]}, indent=2) + "\n")
        base.with_suffix(".md").write_text(md)
        print("\n" + md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
