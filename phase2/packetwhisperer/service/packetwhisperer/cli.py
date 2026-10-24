"""Command line: packetwhisperer {diagnose,extract,serve,usage,mcp}."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _cmd_extract(args: argparse.Namespace) -> int:
    from packetwhisperer.extract import compact, extract

    ex = extract(args.pcap)
    if args.text:
        print(ex.text)
    else:
        print(json.dumps(compact(ex.summary) if args.compact else ex.summary, indent=2))
    print(f"analyzed {ex.packets} packets in {ex.seconds:.3f}s ({ex.packets_per_second:,.0f} packets/s)",
          file=sys.stderr)
    return 0


def _cmd_diagnose(args: argparse.Namespace) -> int:
    from packetwhisperer.diagnose import diagnose
    from packetwhisperer.extract import extract

    d = diagnose(extract(args.pcap).summary, args.engine, context=args.context)
    if args.json:
        print(d.model_dump_json(indent=2))
        return 0
    print(f"{d.label} ({d.confidence} confidence, {d.engine})\n\n{d.summary}\n\nEvidence:")
    print("\n".join(f"  - {e}" for e in d.evidence))
    print("Next steps:")
    print("\n".join(f"  - {s}" for s in d.next_steps))
    if d.note:
        print(f"\nNote: {d.note}")
    return 0


def _cmd_serve(args: argparse.Namespace) -> int:
    import uvicorn

    uvicorn.run("packetwhisperer.api:app", host=args.host, port=args.port)
    return 0


def _cmd_usage(args: argparse.Namespace) -> int:
    from packetwhisperer.usage import read_entries, summarize

    print(json.dumps(summarize(read_entries(Path(args.log))), indent=2))
    return 0


def _cmd_mcp(_: argparse.Namespace) -> int:
    from packetwhisperer.mcp_server import main as mcp_main

    mcp_main()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="packetwhisperer", description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("diagnose", help="diagnose a pcap")
    p.add_argument("pcap")
    p.add_argument("--engine", choices=["auto", "rules", "llm"], default="auto")
    p.add_argument("--context", help="what the user reported, e.g. 'wifi slow in library'")
    p.add_argument("--json", action="store_true")
    p.set_defaults(fn=_cmd_diagnose)

    p = sub.add_parser("extract", help="print the analyzer summary")
    p.add_argument("pcap")
    p.add_argument("--text", action="store_true", help="human-readable report instead of JSON")
    p.add_argument("--compact", action="store_true", help="the trimmed JSON the LLM sees")
    p.set_defaults(fn=_cmd_extract)

    p = sub.add_parser("serve", help="run the HTTP API")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8000)
    p.set_defaults(fn=_cmd_serve)

    p = sub.add_parser("usage", help="summarize the usage log")
    p.add_argument("--log", default="usage.jsonl")
    p.set_defaults(fn=_cmd_usage)

    p = sub.add_parser("mcp", help="run as an MCP server over stdio")
    p.set_defaults(fn=_cmd_mcp)

    args = parser.parse_args(argv)
    try:
        return args.fn(args)
    except (RuntimeError, OSError) as e:
        print(f"packetwhisperer: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
