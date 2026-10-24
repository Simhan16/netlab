# 00-healthy

**Label:** `healthy`

## Root cause
Nothing is wrong. This is the control group.

## Expected signature in the mini-tcpdump summary
- TCP flows end `closed` (both FINs) with 0 or near-0 retransmissions
- DNS queries all answered NOERROR, latency in single/low double-digit ms
- No ICMP errors

## Real-world fix
N/A. A good diagnoser must say *healthy* here, not invent a problem.

## Observed
<!-- Fill in after `sudo ./run_case.sh 00-healthy`: paste the relevant lines from
`mini-tcpdump -r pcaps/00-healthy/run1.pcap -q` and note anything that differed from the expected signature. -->
