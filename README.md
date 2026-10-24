# netlab

Networking fundamentals built from scratch in C++ on Linux. That work then becomes an
AI-powered diagnostics tool deployed for real users. The plan and the rules for working
in this repo are in [CLAUDE.md](CLAUDE.md).

```
phase1/                         network fundamentals (~12 weeks)
├── netns-router/   1.1  two LANs + router + NAT + firewall in network namespaces (bash)
├── common/              shared C++: RAII fd, socket helpers, bounds-checked byte reader, test macros
├── echo-server/    1.2  blocking → epoll TCP echo server, bench client
├── dns-resolver/   1.3  dnsq: hand-built DNS packets, TCP fallback, iterative +trace from the root
├── udp-ping/       1.3  UDP latency/jitter/loss probe + echo server
├── mini-tcpdump/   1.4  libpcap capture + full decode + per-flow TCP health  ← flagship C++
└── casebook/       1.5  10 fault-injection cases → labeled pcaps (the Phase 2 eval set)
phase2/                         FDE integration (~10–12 weeks)
├── packetwhisperer/ 2.1 pcap → C++ parser (pybind11) → Claude → plain-English root cause
│   └── evals/           accuracy per failure type against the casebook
├── deploy/         2.2/2.3  Docker + Caddy + EC2, Slack alerts, pilot playbook
└── CASE_STUDY.md   2.4  the one-pager (fill in from real numbers)
```

## Quick start (Ubuntu, e.g. the EC2 box)
```bash
sudo apt install -y build-essential cmake libpcap-dev python3-venv curl openssl
cd phase1 && cmake -B build && cmake --build build -j && (cd build && ctest)   # all C++ tools + tests
sudo netns-router/setup.sh && sudo netns-router/test.sh                       # the lab
sudo casebook/run_case.sh all 3                                               # 30 labeled captures

cd ../phase2/packetwhisperer && python3 -m venv .venv && . .venv/bin/activate
pip install -e ".[dev,mcp]" && pytest
python evals/run_evals.py                                                     # rules baseline on the casebook
```
macOS builds everything except `echo_server_epoll`. The netns lab and live capture need Linux and root.

## Status
| Step | Built | Verified on | Still to do |
|---|---|---|---|
| 1.1 netns-router | ✅ | syntax only (needs Linux) | run on EC2, paste `test.sh` output into its README |
| 1.2 echo-server | ✅ | blocking server + client on macOS | epoll server on Linux; bench numbers from EC2 |
| 1.3 dns-resolver, udp-ping | ✅ | live queries + `+trace` + unit tests on macOS | |
| 1.4 mini-tcpdump | ✅ | unit tests, `-r` on generated pcaps | live capture on EC2; packets/sec on a big pcap |
| 1.5 casebook | ✅ scripts | syntax only | **run it**: `labels.json` is empty until you do |
| 2.1 PacketWhisperer | ✅ | 29 tests (synthetic pcaps, mocked API) | first real LLM run; evals on the real casebook |
| 2.2 pilot | 📝 playbook | n/a | find a user, scope with them |
| 2.3 deploy | ✅ files | not built (no Docker daemon here) | `docker compose up` on EC2 |
| 2.4 case study | 📝 template | n/a | fill in from measured numbers |

## Resume entry
See the bottom of [CLAUDE.md](CLAUDE.md). Every `[X]` there maps to a measurement above.
