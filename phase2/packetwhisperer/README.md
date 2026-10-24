# PacketWhisperer

**Upload a pcap, get a plain-English root cause.** AI-powered network diagnostics for
people who don't read packet captures: campus IT help desks, student org admins, and
on-call engineers at 2 a.m. Phase 2.1.

```
pcap ──► C++ parser (pktcore, from mini-tcpdump) ──► flow summary JSON ──► diagnoser ──► diagnosis
          pybind11, GIL released                       compacted:            rules (baseline)
                                                       problems first        Claude (structured output)
```

| Layer | Where | What |
|---|---|---|
| Extraction | `core/` + `bindings/` | Builds `phase1/mini-tcpdump`'s `pktcore` from the same sources, exposed as `packetwhisperer._pktcore` |
| Analysis | `service/packetwhisperer/llm.py` | Claude reads the compacted summary, never raw bytes, and must answer in a JSON schema whose `label` is an enum of `taxonomy.py` |
| Baseline | `service/packetwhisperer/rules.py` | Hand-written signatures. The LLM has to beat these in evals to justify its cost; they're also the fallback when the API is down |
| Service | `api.py`, `cli.py`, `mcp_server.py` | FastAPI upload API + web page, CLI, and an MCP server so an AI assistant can call it |
| Evals | `evals/run_evals.py` | Accuracy per failure type against the Phase 1 casebook |

## Setup
```bash
sudo apt install -y build-essential cmake libpcap-dev python3-venv   # Ubuntu (macOS: Xcode CLT has libpcap)
cd phase2/packetwhisperer
python3 -m venv .venv && . .venv/bin/activate
pip install -e ".[dev,mcp]"        # compiles the C++ extension via scikit-build-core
pytest                              # synthetic pcaps; no API key or root needed
export ANTHROPIC_API_KEY=...        # for the LLM engine
```

## Use
```bash
packetwhisperer diagnose capture.pcap                       # auto: Claude, falls back to rules
packetwhisperer diagnose capture.pcap --engine rules
packetwhisperer diagnose capture.pcap --context "wifi in the library drops video calls"
packetwhisperer extract capture.pcap --compact              # exactly what the LLM sees
packetwhisperer serve --port 8000                           # API + upload page at http://localhost:8000
curl -F file=@capture.pcap -H "X-API-Key: $KEY" localhost:8000/v1/diagnose
claude mcp add packetwhisperer -- packetwhisperer mcp      # let Claude Code call it
```

Example response (the shape is real; the values here are illustrative):
```json
{"request_id": "3f9c0a1b2c4d",
 "diagnosis": {"label": "firewall_drop", "confidence": "high", "engine": "llm",
   "summary": "Your computer keeps trying to connect but gets no answer at all...",
   "evidence": ["192.168.20.10:51000 -> 192.168.10.10:8080: 4 SYNs over 7.0s, no reply"],
   "next_steps": ["Check security groups and host firewalls for a rule dropping port 8080"]},
 "capture": {"packets": 4, "duration_s": 7.0, ...}, "latency_ms": 2210.4}
```

## Configuration (environment)
| Variable | Default | |
|---|---|---|
| `ANTHROPIC_API_KEY` | (none) | needed for `llm`/`auto`; without it `auto` uses rules |
| `PW_API_KEYS` | (empty: auth off) | comma-separated keys accepted in `X-API-Key` |
| `PW_MODEL` / `PW_EFFORT` | `claude-opus-5-5` / `medium` | |
| `PW_ENGINE` | `auto` | default engine for API requests |
| `PW_MAX_UPLOAD_MB` | `50` | |
| `PW_USAGE_LOG` | `usage.jsonl` | one JSON line per request; `packetwhisperer usage` summarizes it |
| `SLACK_WEBHOOK_URL` | (none) | alert on non-healthy diagnoses (Discord works via its `/slack` webhook suffix) |

## Design decisions
- **Summaries, not bytes.** A 50 MB capture is millions of tokens of hex; its summary is a few thousand. The parser computes what a human expert would compute first: states, retransmissions, RTT per direction, DNS outcomes, ICMP errors. The model then reasons over that.
- **Problems first.** `compact()` sorts flows by an interest score and cuts the rest, saying how many it cut, so a single broken flow among 10,000 healthy ones survives the trim.
- **Closed label set.** Structured output with an enum makes every answer scoreable against the casebook. Free text can't be evaluated.
- **Degrade, don't fail.** `auto` falls back to the rules engine on API errors, rate limits, or a refusal, and says so in `note`. Server-side refusal fallback (`fallbacks: "default"`) is enabled, because captures of scans or attacks are legitimate inputs here.
- **Privacy.** Uploads go to a temp directory that's deleted after analysis. Only the summary is sent to the API, and it contains IPs, ports and DNS names. Tell pilot users that up front.

## Known limitations
- The rules' thresholds are tuned on lab captures. Expect misses on real-world traffic until the casebook includes some.
- One primary label per capture. Real outages sometimes have two causes.
- pcapng needs libpcap ≥ 1.1 (any modern distro has it). Encrypted payloads stay opaque, by design.
- The MCP server reads local files and is meant for a single trusted user over stdio.
