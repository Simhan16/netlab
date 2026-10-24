# CLAUDE.md — Networking + FDE Project Handoff

## Context
- Learner: undergraduate CS major, ~1 year before graduation.
- Career goal: Forward Deployed Engineer (FDE) roles (Palantir, AI labs, startup "founding/solutions engineer").
- FDE signals to build: ship working software fast, scope messy real-world problems with users, communicate clearly with non-engineers.
- Resume framing for everything: **problem → user → what was shipped → impact (with numbers)**.

## How Claude should work in this repo
- Teach while building: explain *why* for networking concepts, not just the code.
- Prefer small, runnable milestones; each phase step should end with something demoable.
- C++: C++17 or later, CMake, `-Wall -Wextra -Werror`, RAII for sockets/handles, no raw `new`/`delete`.
- Python: 3.11+, type hints, `ruff` + `pytest`.
- Every tool gets a README with: purpose, build/run steps, a sample capture/output, and known limitations.
- Never fabricate metrics; leave `[X]` placeholders until measured.

---

## Phase 1 — Network fundamentals on Linux, C++, packet analysis (~12 weeks)

### 1.1 Linux networking basics
- Practice: `ip addr/route/link`, bridges, veth pairs, `iptables`/`nftables`, NAT, `ss`.
- Mini-project: **namespace router** — two "LANs" in network namespaces, NAT to the internet, firewall rules between them. Deliver as a reproducible shell script.

### 1.2 C++ sockets from scratch
- TCP echo server + client with POSIX sockets (`socket`, `bind`, `listen`, `accept`, `connect`).
- Upgrade the server to handle many clients with `epoll`.
- Capture it with tcpdump and annotate the handshake/teardown.

### 1.3 Protocols hands-on (+ C++ implementations)
- Observe live: ARP, DHCP, DNS, TCP, TLS, HTTP/2.
- Run Unbound/Bind DNS and a WireGuard tunnel between two VMs.
- C++: **minimal DNS resolver** (hand-built query packets, parse responses, network byte order).
- C++: **UDP ping tool** measuring latency and loss.

### 1.4 tcpdump / Wireshark analysis
- BPF filters, follow TCP streams, read retransmissions, window sizes, RSTs.
- Use `tc netem` to inject latency/loss and observe TCP behavior.
- C++: **mini-tcpdump** with libpcap — capture, BPF filters, parse Ethernet/IP/TCP/UDP headers, per-flow stats (retransmissions, RTT estimates). Flagship C++ project of Phase 1.

### 1.5 Phase 1 capstone — Broken-Network Casebook
- Deliberately break things: DNS misconfig, MTU black hole, TLS SNI mismatch, asymmetric routing, etc.
- Capture each failure (using mini-tcpdump), write a short diagnosis per case.
- Publish as labeled pcaps + `labels.json`. **This becomes the eval set in Phase 2.**

---

## Phase 2 — FDE integration (~10–12 weeks)

### 2.1 PacketWhisperer (flagship)
AI-powered network diagnostics: upload a pcap → plain-English root cause.
- Extraction layer: Phase 1 C++ parser, exposed to Python via **pybind11**.
- Analysis layer: LLM prompt over extracted flow summaries (not raw bytes).
- Evals: run against the Casebook; report accuracy per failure type.

### 2.2 Deploy for a real user
- Pair with a campus Wi-Fi diagnostics bot or PingPong uptime monitor for campus IT or a student org.
- Scope requirements with them, ship, iterate on feedback, log usage.

### 2.3 Integration polish
- Docker, auth, Slack/Discord alerts, cloud hosting.
- Optional: expose diagnostics as an **MCP server** so an AI assistant can call them.

### 2.4 Case study
One page: problem → user → shipped → impact (tickets deflected, accuracy, triage time saved).

---

## Suggested repo layout
```
.
├── CLAUDE.md
├── phase1/
│   ├── netns-router/        # 1.1 shell scripts
│   ├── echo-server/         # 1.2 C++ (epoll)
│   ├── dns-resolver/        # 1.3 C++
│   ├── udp-ping/            # 1.3 C++
│   ├── mini-tcpdump/        # 1.4 C++ + libpcap
│   └── casebook/            # 1.5 pcaps + labels.json + diagnoses
└── phase2/
    ├── packetwhisperer/
    │   ├── core/            # C++ parser (shared with mini-tcpdump)
    │   ├── bindings/        # pybind11
    │   ├── service/         # Python API + LLM analysis
    │   └── evals/           # runs against phase1/casebook
    └── deploy/              # Dockerfiles, IaC, alerting
```

## Target resume entry (fill in real numbers)
**PacketWhisperer — AI-Powered Network Diagnostics** | C++, libpcap, Python, pybind11, LLM APIs, Docker, AWS
- Built a high-performance C++ packet capture and parsing engine (libpcap, epoll) decoding Ethernet/IP/TCP/UDP/DNS with per-flow retransmission and RTT stats, processing [X]K packets/sec.
- Developed an LLM root-cause analyzer turning pcaps into plain-English diagnoses, reaching [X]% accuracy on a self-built eval set of [N] labeled failure captures.
- Integrated the C++ engine into a Python service via pybind11; containerized and deployed on AWS with Slack alerting and auth.
- Partnered with [campus IT / student org] to deploy to [N] users, cutting network-issue triage time from [X] to [Y] min.

## Backlog — alternative / follow-on projects
| Project | Stack | Signal |
|---|---|---|
| FlowGuard — real-time anomaly detector (scans, SYN floods, DNS tunneling) | C++, libpcap, Grafana | Systems + security |
| ZeroTrust-Lite — identity-aware reverse proxy | C++/Go, OIDC, TLS | Infra + real users |
| MeshCast — P2P file sync over UDP with custom congestion control | C++ | C++ depth |
| NetCopilot — MCP server to chat with network devices | Python, MCP, SNMP/gNMI | AI integration (FDE) |
| LoadLens — L7 load balancer with live visualization | C++, epoll | C++ depth |
| CampusPulse — crowdsourced connectivity heatmap | React Native, FastAPI, PostGIS | Real users (FDE) |
| TLSight — certificate/TLS health scanner | C++, OpenSSL | Practical ops tool |

Pairing advice: one AI/user-facing project (NetCopilot or CampusPulse) + one C++-depth project (MeshCast or LoadLens).

## Where to start
Follow the practice order in README.md: 1.0 tcpdump-basics and 1.0b socket-basics come first, then 1.1 (namespace router) and 1.2 (epoll echo server). Ask the learner which Linux environment they have (native, WSL2, or VM) before writing setup scripts.
