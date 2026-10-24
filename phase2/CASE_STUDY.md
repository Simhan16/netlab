# Case study: PacketWhisperer

*One page. Fill in from the pilot log and eval results, and leave `[X]` until measured. Phase 2.4.*

## Problem
[Who] spends [X] minutes on each network complaint. Most of that time goes to working out
*which* layer is broken (DNS? firewall? the Wi-Fi?) before anyone can fix anything.
Reading packet captures is the fastest way to know, but only [N] people on the team can do it.

## User
[Team / org], [N] staff handling about [X] network tickets a week. Scoped with [role] on [date].
Key constraints: [e.g. no data off campus without approval; staff aren't Wireshark users].

## What I shipped
- A C++ capture/parsing engine (libpcap) decoding Ethernet → IP → TCP/UDP/DNS/ICMP/TLS with per-flow retransmission, RTT and connection-outcome analysis, processing [X]K packets/sec on [instance type].
- An LLM root-cause analyzer (Claude, structured output over a closed set of [N] root causes), with a rule-based baseline and automatic fallback.
- Integration via pybind11 into a FastAPI service with API-key auth, a web upload page, Slack alerts and a usage log; deployed with Docker on AWS behind HTTPS.
- A labeled eval set of [N] real failure captures across [N] failure types, built with a network-namespace fault-injection lab.

## Impact
| Metric | Before | After |
|---|---|---|
| Median triage time | [X] min | [Y] min |
| Diagnosis accuracy (eval set, held-out runs) | rules: [X]% | LLM: [Y]% |
| Diagnoses users marked correct (pilot) | n/a | [X]% of [N] |
| Weekly active users | 0 | [N] |

## What I learned
- [Something the users needed that you didn't predict]
- [A technical surprise, e.g. why vantage point matters for the same fault]
- [What you'd do differently]
