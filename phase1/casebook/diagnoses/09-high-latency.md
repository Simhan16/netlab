# 09-high-latency

**Label:** `high_latency`

## Root cause
Every round trip is slow (long path, satellite/VPN hop, bufferbloat, netem here). No loss.

## Expected signature in the mini-tcpdump summary
- `handshake_ms` and `rtt_ms` all ≈ 300+ ms, consistently
- 0 retransmissions
- Flows complete, just slowly

## Real-world fix
Move the service closer (region/CDN), cut round trips (connection reuse, HTTP/2, TLS 1.3), fix bufferbloat (fq_codel/cake).

## Observed
<!-- Fill in after `sudo ./run_case.sh 09-high-latency`: paste the relevant lines from
`mini-tcpdump -r pcaps/09-high-latency/run1.pcap -q` and note anything that differed from the expected signature. -->
