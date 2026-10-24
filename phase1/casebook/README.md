# Broken-Network Casebook

A labeled set of real failure captures. Each case breaks the netns lab in one known way,
captures the failure with mini-tcpdump, and records the ground-truth label. This is the
**eval set for PacketWhisperer** (Phase 2), so a diagnosis's accuracy is measured against
failures you caused and understand, not against vibes. Phase 1.5.

| Case | Label | What breaks | Capture point |
|---|---|---|---|
| 00-healthy | `healthy` | nothing (the control group) | lan-a |
| 01-dns-unreachable | `dns_unreachable` | resolver IP is dead | lan-a |
| 02-dns-nxdomain | `dns_nxdomain` | hostname doesn't exist | lan-a |
| 03-mtu-blackhole | `mtu_blackhole` | 1200-byte hop + ICMP frag-needed filtered | router uplink |
| 04-tls-handshake-failure | `tls_handshake_failure` | cert for the wrong hostname | lan-a |
| 05-asymmetric-routing | `asymmetric_routing` | replies take another path, dropped by uRPF | lan-b (server) |
| 06-firewall-drop | `firewall_drop` | SYNs silently dropped | lan-b (client) |
| 07-connection-refused | `connection_refused` | no listener on the port | lan-a |
| 08-packet-loss | `packet_loss` | 8% random loss (netem) | lan-a |
| 09-high-latency | `high_latency` | +300 ms delay (netem) | lan-a |

`diagnoses/<case>.md` explains each root cause, the signature to expect in the capture,
and the real-world fix. Add your own observations there after you run each case.

## Run
```bash
sudo ../netns-router/setup.sh                 # the lab
(cd .. && cmake -B build && cmake --build build -j)   # mini-tcpdump
sudo apt install -y linux-modules-extra-$(uname -r)   # netem for cases 08/09, if tc complains

sudo ./run_case.sh list
sudo ./run_case.sh 03-mtu-blackhole           # one case
sudo ./run_case.sh all 5                      # every case, 5 runs each → 50 labeled pcaps
../build/mini-tcpdump/mini-tcpdump -r pcaps/03-mtu-blackhole/run1.pcap -q
```

Each run writes `pcaps/<case>/runN.pcap` and upserts an entry in `labels.json`:
```json
{"id": "03-mtu-blackhole-run1", "pcap": "pcaps/03-mtu-blackhole/run1.pcap", "label": "mtu_blackhole",
 "case": "03-mtu-blackhole", "capture_point": "router:r-up", "synthetic": false, "captured_at": "...", "description": "..."}
```
`labels.json` starts empty on purpose. Every entry should be a capture you actually took.

## Adding a case
Copy a file in `cases/`, set `CASE_LABEL` (add new labels to
`phase2/packetwhisperer/service/packetwhisperer/taxonomy.py` too), and write `case_break`,
`case_trigger` and `case_fix`. `case_fix` must fully undo `case_break`. The runner calls it
from an EXIT trap even when the trigger fails, so the lab is never left broken.

Ideas: DNS over TCP blocked with large responses, a TTL-exceeded routing loop, a duplicate
IP (ARP conflict), a zero-window slow consumer, an expired certificate, an HTTP proxy
returning 502.

## Known limitations
- One vantage point per case. The same fault looks different from the client, the server and the router. That's realistic, but it's also why `capture_point` is recorded.
- Lab conditions are cleaner than production: there's no background traffic. For a harder eval, mix a healthy flow into each case.
- Case 00 and the DNS cases touch the real internet (example.com, your resolver). The others are fully local.
