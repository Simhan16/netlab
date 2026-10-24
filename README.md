# netlab

Networking fundamentals built from scratch in C++ on Linux. That work then becomes an
AI-powered diagnostics tool deployed for real users. The plan and the rules for working
in this repo are in [CLAUDE.md](CLAUDE.md).

```
phase1/                         network fundamentals (~12 weeks)
├── tcpdump-basics/ 1.0  start here: MAC/IP/port addressing, reading tcpdump, Wireshark on the Mac
├── socket-basics/  1.0b write clients and servers yourself: TCP, framing, UDP, poll() (starters + checker)
├── netns-router/   1.1  two LANs + router + NAT + firewall in network namespaces (bash)
├── common/              shared C++: RAII fd, socket helpers, bounds-checked byte reader, test macros
├── echo-server/    1.2  blocking → epoll TCP echo server, bench client
├── dns-resolver/   1.3  dnsq: hand-built DNS packets, TCP fallback, iterative +trace from the root
├── udp-ping/       1.3  UDP latency/jitter/loss probe + echo server
├── mini-tcpdump/   1.4  libpcap capture + full decode + per-flow TCP health  ← flagship C++
├── casebook/       1.5  10 fault-injection cases → labeled pcaps (the Phase 2 eval set)
└── overlays/       1.6  advanced: VLANs + bridges, GRE, VXLAN, WireGuard, overlay MTU and MSS clamping
phase2/                         FDE integration (~10–12 weeks)
├── packetwhisperer/ 2.1 pcap → C++ parser (pybind11) → Claude → plain-English root cause
│   └── evals/           accuracy per failure type against the casebook
├── deploy/         2.2/2.3  Docker + Caddy + EC2, Slack alerts, pilot playbook
└── CASE_STUDY.md   2.4  the one-pager (fill in from real numbers)
```

## Practice in this order
Each stage uses what the one before it taught. The folder layout above groups code by topic,
so follow this list, not the tree. Each chapter's README has the concepts, exercises and
"check yourself" questions. A stage is done when its **Done when** line is true.

**Where things run:** 🍎 = your Mac works (no root needed), 🐧 = the Ubuntu EC2 box (root needed
for the lab and live capture). Build once per machine: `cd phase1 && cmake -B build && cmake --build build -j`.

### Phase 1: network fundamentals
| # | Chapter | Where | Do | Done when |
|---|---|---|---|---|
| 1 | [tcpdump-basics](phase1/tcpdump-basics/README.md) | 🍎, then 🐧 for ex. 5 | Parts 1-3 (MAC/IP/port addressing, tcpdump, Wireshark), then exercises 1-5 | you can read any line of `tcpdump -n` output aloud, and you've filled in exercise 5's table from your own EC2 capture |
| 2 | [socket-basics](phase1/socket-basics/README.md) | 🍎 or 🐧 | steps 1→6 in order, each in `exercises/` | `python3 socket-basics/check.py <step>` passes for every step. Then compare yours with `solutions/` |
| 3 | [netns-router](phase1/netns-router/README.md) | 🐧 | `sudo ./setup.sh && sudo ./test.sh`, then exercises 1-4 | `test.sh` passes, and you can explain why each exercise broke what it broke. Paste the output into its README |
| 4 | [echo-server](phase1/echo-server/README.md) | 🍎 ex. 1-2, 🐧 ex. 3-4 | read `echo_server_blocking.cpp` (socket-basics step 3, grown up), then `echo_server_epoll.cpp` (step 6 with epoll); exercises 1-4 | you've annotated a handshake capture and seen both zero windows in exercise 3 |
| 5 | [dns-resolver](phase1/dns-resolver/README.md) | 🍎 or 🐧 | read `src/dns.cpp` (wire format), exercises 1-4 | you can explain a `+trace` run hop by hop |
| 6 | [udp-ping](phase1/udp-ping/README.md) | 🐧 (the lab) | exercises 1-3 with netem | your measured loss and RTT match what you injected, and you can say why exercise 3's loss nearly doubles |
| 7 | [mini-tcpdump](phase1/mini-tcpdump/README.md) | 🍎 `-r` on files, 🐧 live | read `src/core/` layer by layer, then exercises 1-4 | its per-flow report matches what you see in Wireshark for the same pcap |
| 8 | [casebook](phase1/casebook/README.md) | 🐧 | `sudo casebook/run_case.sh all 5` (5 runs per case, so the evals can hold some out); for each case, diagnose the capture yourself *before* reading `diagnoses/<case>.md` | `labels.json` has 50 captures, and you've added your own observations to each diagnosis |
| 9 | [overlays](phase1/overlays/README.md) | 🐧 | advanced, and it can run alongside Phase 2: `sudo ./setup.sh`, then exercises 1-4 (VLANs), then 5-9, switching tunnels with `./tunnel.sh` | `test.sh` passes in all four tunnel modes, and you can draw each tunnel's headers and explain its MTU |

Two habits for every stage after 1:
- **Capture it.** Whenever a program sends something, watch it with tcpdump or Wireshark. The
  exercises tell you where to look, but make it a reflex.
- **Answer the "check yourself" questions before opening the answers.** They're the part
  interviewers ask about.

### Phase 2: the FDE project (needs stage 8's captures)
| # | Part | Where | Do | Done when |
|---|---|---|---|---|
| 10 | [PacketWhisperer](phase2/packetwhisperer/README.md) | 🍎 or 🐧 | set it up, `pytest`, then diagnose a few casebook pcaps with the rules engine only (no API key) | tests pass, and you know which casebook cases the rules get right |
| 11 | [evals](phase2/packetwhisperer/evals/README.md) | 🐧 | rules baseline on the real casebook, then one LLM run (it costs API credits: check the README first) | per-label accuracy numbers recorded, from real runs only |
| 12 | [deploy](phase2/deploy/README.md) | 🐧 | `docker compose up` behind Caddy, then Slack alerts | the service answers over HTTPS from your laptop |
| 13 | [pilot](phase2/deploy/PILOT.md) | n/a | find one real user and scope a problem with them | they've used it on a real problem at least once |
| 14 | [case study](phase2/CASE_STUDY.md) | n/a | fill in every `[X]` from measured numbers | no placeholders left. Then update the resume entry in CLAUDE.md |

## Quick start (Ubuntu, e.g. the EC2 box)
```bash
sudo apt install -y build-essential cmake libpcap-dev python3-venv curl openssl
cd phase1 && cmake -B build && cmake --build build -j && (cd build && ctest)   # all C++ tools + tests
sudo netns-router/setup.sh && sudo netns-router/test.sh                       # the lab
sudo casebook/run_case.sh all 3                                               # 30 labeled captures
sudo overlays/setup.sh && sudo overlays/test.sh                               # VLAN + tunnel lab (isolated)

cd ../phase2/packetwhisperer && python3 -m venv .venv && . .venv/bin/activate
pip install -e ".[dev,mcp]" && pytest
python evals/run_evals.py                                                     # rules baseline on the casebook
```
macOS builds everything except `echo_server_epoll`. The netns lab and live capture need Linux and root.

## Status
| Step | Built | Verified on | Still to do |
|---|---|---|---|
| 1.0 tcpdump-basics | ✅ | practice.pcap checked with tcpdump + tshark on macOS | exercise 5 on EC2 |
| 1.0b socket-basics | ✅ | all 28 checks pass on the solutions (macOS); 11 deliberately broken versions each caught by the intended check | run `check.py all --solution` on Linux |
| 1.1 netns-router | ✅ | syntax only (needs Linux) | run on EC2, paste `test.sh` output into its README |
| 1.2 echo-server | ✅ | blocking server + client on macOS | epoll server on Linux; bench numbers from EC2 |
| 1.3 dns-resolver, udp-ping | ✅ | live queries + `+trace` + unit tests on macOS | |
| 1.4 mini-tcpdump | ✅ | unit tests, `-r` on generated pcaps | live capture on EC2; packets/sec on a big pcap |
| 1.5 casebook | ✅ scripts | syntax only | **run it**: `labels.json` is empty until you do |
| 1.6 overlays | ✅ scripts | `bash -n` + shellcheck only (needs Linux) | run `test.sh` in all four tunnel modes on EC2, paste the output into its README |
| 2.1 PacketWhisperer | ✅ | 29 tests (synthetic pcaps, mocked API) | first real LLM run; evals on the real casebook |
| 2.2 pilot | 📝 playbook | n/a | find a user, scope with them |
| 2.3 deploy | ✅ files | not built (no Docker daemon here) | `docker compose up` on EC2 |
| 2.4 case study | 📝 template | n/a | fill in from measured numbers |

## Resume entry
See the bottom of [CLAUDE.md](CLAUDE.md). Every `[X]` there maps to a measurement above.
