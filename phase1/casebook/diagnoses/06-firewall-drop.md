# 06-firewall-drop

**Label:** `firewall_drop`

## Root cause
A firewall silently DROPs the traffic (no RST, no ICMP). The client can't tell 'blocked' from 'host down'.

## Expected signature in the mini-tcpdump summary
- State `syn_unanswered` with `syns` ≥ 3 at growing intervals (1s, 2s, 4s… exponential backoff)
- Zero packets from the server
- No ICMP errors (a REJECT rule would send RST or ICMP admin-prohibited instead)

## Real-world fix
Find the rule (security group, NACL, host firewall, network ACL) and allow the flow if it should be allowed. Use REJECT internally so failures are fast and obvious.

## Observed
<!-- Fill in after `sudo ./run_case.sh 06-firewall-drop`: paste the relevant lines from
`mini-tcpdump -r pcaps/06-firewall-drop/run1.pcap -q` and note anything that differed from the expected signature. -->
