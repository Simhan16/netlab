"""MCP server: lets an AI assistant (Claude Code, Claude Desktop, ...) analyze pcaps.

Runs over stdio on the user's machine and reads local files, so it is meant
for a single trusted user; don't expose it over the network as-is.

    claude mcp add packetwhisperer -- packetwhisperer mcp
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from mcp.server.mcpserver import MCPServer

from packetwhisperer.diagnose import diagnose
from packetwhisperer.extract import compact, extract

PCAP_SUFFIXES = {".pcap", ".pcapng", ".cap"}

server = MCPServer(
    "packetwhisperer",
    instructions=(
        "Tools for analyzing packet capture files on this machine. Use summarize_pcap to get per-flow "
        "TCP, DNS, ICMP and TLS statistics, and diagnose_pcap for a root-cause guess."
    ),
)


def _checked_path(path: str) -> Path:
    p = Path(path).expanduser().resolve()
    if p.suffix.lower() not in PCAP_SUFFIXES:
        raise ValueError(f"expected a {', '.join(sorted(PCAP_SUFFIXES))} file, got {p.name}")
    if not p.is_file():
        raise ValueError(f"no such file: {p}")
    return p


@server.tool()
def summarize_pcap(path: str, max_flows: int = 20) -> dict[str, Any]:
    """Decode a pcap file and return flow-level statistics: TCP connection states, retransmissions,
    RTTs, DNS queries and failures, ICMP errors, TLS SNI and alerts. Most suspicious flows first."""
    return compact(extract(_checked_path(path)).summary, max_flows=max_flows)


@server.tool()
def diagnose_pcap(path: str, engine: str = "rules", context: str | None = None) -> dict[str, Any]:
    """Diagnose the most likely network root cause in a pcap file. engine='rules' is instant and free;
    engine='llm' asks Claude (needs ANTHROPIC_API_KEY). `context` is the user's description of the problem."""
    if engine not in ("rules", "llm", "auto"):
        raise ValueError("engine must be 'rules', 'llm' or 'auto'")
    return diagnose(extract(_checked_path(path)).summary, engine, context=context).model_dump()  # type: ignore[arg-type]


def main() -> None:
    server.run("stdio")


if __name__ == "__main__":
    main()
